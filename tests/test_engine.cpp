#define CATCH_CONFIG_MAIN
#include <new> // Catch2 v2 requires std::nothrow with newer libc++ headers.
#include <catch2/catch.hpp>
#include "matchengine/engine.hpp"

#include <algorithm>
#include <limits>
#include <random>

using namespace matchengine;

static Order order(OrderID id, Side side, double price, Quantity qty,
                   TraderID trader = 0, OrderType type = OrderType::LIMIT) {
    Order o;
    o.order_id = id; o.trader_id = trader == 0 ? id : trader;
    o.side = side; o.price = price; o.quantity = qty; o.type = type;
    return o;
}

TEST_CASE("Empty book and first limit order") {
    Engine e;
    REQUIRE_FALSE(e.get_top_of_book().best_bid);
    REQUIRE_FALSE(e.get_top_of_book().best_ask);
    REQUIRE(e.add_order(order(1, Side::BUY, 100, 10)).empty());
    REQUIRE(e.get_order_status(1) == OrderStatus::NEW);
    REQUIRE(e.get_top_of_book().best_bid == 100);
    REQUIRE(e.get_top_of_book().bid_qty == 10);
    REQUIRE(e.get_book_snapshot(0).bids.empty());
}

TEST_CASE("Exact matches execute at resting price in both directions") {
    const auto side = GENERATE(Side::BUY, Side::SELL);
    Engine e;
    e.add_order(order(1, side, 100, 10));
    auto trades = e.add_order(order(2, side == Side::BUY ? Side::SELL : Side::BUY, 100, 10));
    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].price == 100);
    REQUIRE(trades[0].quantity == 10);
    REQUIRE(trades[0].resting_order_id == 1);
    REQUIRE(trades[0].aggressor_order_id == 2);
    REQUIRE(trades[0].aggressor_side != side);
    REQUIRE(trades[0].timestamp > 0);
    REQUIRE(trades[0].trade_id == 1);
    REQUIRE(e.get_order_status(1) == OrderStatus::FILLED);
    REQUIRE(e.get_order_status(2) == OrderStatus::FILLED);
    REQUIRE(e.resting_order_count() == 0);
    REQUIRE_FALSE(e.get_top_of_book().best_bid);
    REQUIRE_FALSE(e.get_top_of_book().best_ask);
}

TEST_CASE("Partial fill sweeps best prices before worse prices") {
    const auto side = GENERATE(Side::BUY, Side::SELL);
    const auto opposite = side == Side::BUY ? Side::SELL : Side::BUY;
    const double direction = side == Side::BUY ? 1 : -1;
    Engine e;
    e.add_order(order(3, opposite, 100 + 2 * direction, 10));
    e.add_order(order(1, opposite, 100, 10));
    e.add_order(order(2, opposite, 100 + direction, 10));
    auto trades = e.add_order(order(4, side, 100 + 2 * direction, 25));
    REQUIRE(trades.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE(trades[i].resting_order_id == i + 1);
        REQUIRE(trades[i].trade_id == i + 1);
        REQUIRE(trades[i].quantity == (i == 2 ? 5 : 10));
    }
    REQUIRE(e.get_order_status(3) == OrderStatus::PARTIALLY_FILLED);
    REQUIRE(e.get_order_status(4) == OrderStatus::FILLED);
    REQUIRE(e.resting_order_count() == 1);
    const auto top = e.get_top_of_book();
    REQUIRE((side == Side::BUY ? top.ask_qty : top.bid_qty) == 5);
}

TEST_CASE("FIFO follows arrival rather than caller timestamps") {
    Engine e;
    auto first = order(1, Side::SELL, 100, 5); first.timestamp = 1000;
    auto second = order(2, Side::SELL, 100, 5); second.timestamp = 1;
    e.add_order(first); e.add_order(second);
    const auto trades = e.add_order(order(3, Side::BUY, 100, 7));
    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].resting_order_id == 1);
    REQUIRE(trades[1].resting_order_id == 2);
    REQUIRE(trades[1].quantity == 2);
    REQUIRE(e.get_top_of_book().ask_qty == 3);
}

TEST_CASE("Self-trade prevention skips own orders at same and worse levels") {
    Engine e;
    e.add_order(order(1, Side::SELL, 99, 10, 7));
    e.add_order(order(2, Side::SELL, 99, 3, 8));
    e.add_order(order(3, Side::SELL, 100, 10, 7));
    e.add_order(order(4, Side::SELL, 101, 5, 9));
    auto trades = e.add_order(order(5, Side::BUY, 101, 9, 7));
    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].resting_order_id == 2);
    REQUIRE(trades[1].resting_order_id == 4);
    REQUIRE(e.get_order_status(1) == OrderStatus::NEW);
    REQUIRE(e.get_order_status(3) == OrderStatus::NEW);
    REQUIRE(e.get_order_status(5) == OrderStatus::PARTIALLY_FILLED);
    REQUIRE(e.get_top_of_book().bid_qty == 1);
    REQUIRE(e.get_top_of_book().best_ask == 99);
}

TEST_CASE("Cancel removes an interior node and leaves other FIFO locations valid") {
    Engine e;
    for (OrderID id = 1; id <= 5; ++id) e.add_order(order(id, Side::SELL, 100, 5));
    REQUIRE(e.cancel_order(3));
    REQUIRE_FALSE(e.cancel_order(3));
    REQUIRE_FALSE(e.cancel_order(999));
    REQUIRE(e.get_order_status(3) == OrderStatus::CANCELLED);
    REQUIRE(e.get_top_of_book().ask_qty == 20);
    auto trades = e.add_order(order(6, Side::BUY, 100, 11));
    REQUIRE(trades.size() == 3);
    REQUIRE(trades[0].resting_order_id == 1);
    REQUIRE(trades[1].resting_order_id == 2);
    REQUIRE(trades[2].resting_order_id == 4);
    REQUIRE(e.cancel_order(4));
    REQUIRE(e.cancel_order(5));
    REQUIRE_FALSE(e.get_top_of_book().best_ask);
}

TEST_CASE("Modify joins tail at the replacement price including unchanged price") {
    const auto original_price = GENERATE(99.0, 100.0);
    Engine e;
    e.add_order(order(1, Side::BUY, original_price, 5));
    e.add_order(order(2, Side::BUY, 100, 5));
    REQUIRE(e.modify_order(1, 100, 7).empty());
    auto trades = e.add_order(order(3, Side::SELL, 100, 12));
    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].resting_order_id == 2);
    REQUIRE(trades[1].resting_order_id == 1);
    REQUIRE(trades[1].quantity == 7);
}

TEST_CASE("Modify can cross and invalid modifications preserve the order") {
    Engine e;
    e.add_order(order(1, Side::BUY, 99, 10));
    e.add_order(order(2, Side::SELL, 100, 5));
    REQUIRE_THROWS_AS(e.modify_order(1, 100, 0), std::invalid_argument);
    REQUIRE(e.get_top_of_book().best_bid == 99);
    REQUIRE(e.get_order_status(1) == OrderStatus::NEW);
    auto trades = e.modify_order(1, 100, 7);
    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].quantity == 5);
    REQUIRE(e.get_top_of_book().bid_qty == 2);
    REQUIRE(e.get_order_status(1) == OrderStatus::PARTIALLY_FILLED);
    REQUIRE_THROWS_AS(e.modify_order(2, 100, 1), std::out_of_range);
    REQUIRE(e.modify_order(1, 99, 4).empty());
    REQUIRE(e.get_top_of_book().bid_qty == 4);
}

TEST_CASE("Market and IOC remainders cancel; IOC respects limit") {
    const auto type = GENERATE(OrderType::MARKET, OrderType::IOC);
    Engine e;
    e.add_order(order(1, Side::SELL, 100, 5));
    e.add_order(order(2, Side::SELL, 101, 5));
    auto trades = e.add_order(order(3, Side::BUY, 100, 20, 3, type));
    REQUIRE(trades.size() == (type == OrderType::MARKET ? 2 : 1));
    REQUIRE(e.get_order_status(3) == OrderStatus::CANCELLED);
    REQUIRE_FALSE(e.get_top_of_book().best_bid);
    Engine empty;
    REQUIRE(empty.add_order(order(1, Side::BUY, 0, 5, 1, OrderType::MARKET)).empty());
    REQUIRE(empty.get_order_status(1) == OrderStatus::CANCELLED);
}

TEST_CASE("FOK rejects atomically excluding own and out-of-limit liquidity") {
    Engine e;
    e.add_order(order(1, Side::SELL, 99, 100, 7));
    e.add_order(order(2, Side::SELL, 100, 5, 8));
    e.add_order(order(3, Side::SELL, 102, 100, 9));
    REQUIRE(e.add_order(order(4, Side::BUY, 100, 6, 7, OrderType::FOK)).empty());
    REQUIRE(e.get_order_status(4) == OrderStatus::REJECTED);
    REQUIRE(e.get_order_status(2) == OrderStatus::NEW);
    REQUIRE(e.get_book_snapshot(3).asks[1].quantity == 5);
    REQUIRE(e.resting_order_count() == 3);
    auto trades = e.add_order(order(5, Side::BUY, 102, 8, 7, OrderType::FOK));
    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].trade_id == 1);
    REQUIRE(trades[0].quantity == 5);
    REQUIRE(trades[1].quantity == 3);
    REQUIRE(e.get_order_status(5) == OrderStatus::FILLED);
    REQUIRE(e.get_order_status(1) == OrderStatus::NEW);
}

TEST_CASE("Snapshot ordering, depth, quantities and cached top") {
    Engine e;
    e.add_order(order(1, Side::BUY, 98, 2));
    e.add_order(order(2, Side::BUY, 99, 3));
    e.add_order(order(3, Side::BUY, 99, 4));
    e.add_order(order(4, Side::SELL, 102, 6));
    e.add_order(order(5, Side::SELL, 101, 5));
    auto snapshot = e.get_book_snapshot(1);
    REQUIRE(snapshot.bids.size() == 1);
    REQUIRE(snapshot.asks.size() == 1);
    REQUIRE(snapshot.bids[0].price == 99);
    REQUIRE(snapshot.bids[0].quantity == 7);
    REQUIRE(snapshot.bids[0].order_count == 2);
    REQUIRE(snapshot.asks[0].price == 101);
    REQUIRE(e.cancel_order(2)); REQUIRE(e.cancel_order(3));
    REQUIRE(e.get_top_of_book().best_bid == 98);
    REQUIRE(e.cancel_order(5));
    REQUIRE(e.get_top_of_book().best_ask == 102);
    REQUIRE(e.get_book_snapshot(99).bids.size() == 1);
}

TEST_CASE("Invalid orders and duplicate IDs cannot corrupt the book") {
    Engine e;
    const auto price = GENERATE(0.0, -1.0, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN());
    REQUIRE(e.add_order(order(1, Side::BUY, price, 5)).empty());
    REQUIRE(e.get_order_status(1) == OrderStatus::REJECTED);
    REQUIRE(e.resting_order_count() == 0);
    REQUIRE_THROWS_AS(e.add_order(order(1, Side::BUY, 100, 5)), std::invalid_argument);
    REQUIRE_THROWS_AS(e.get_order_status(999), std::out_of_range);
    REQUIRE(e.add_order(order(2, Side::BUY, 100, 0)).empty());
    REQUIRE(e.get_order_status(2) == OrderStatus::REJECTED);
    e.add_order(order(3, Side::BUY, 100, 5));
    REQUIRE_THROWS_AS(e.add_order(order(3, Side::SELL, 100, 5)), std::invalid_argument);
    REQUIRE(e.get_top_of_book().bid_qty == 5);
}

// Independent, deliberately slow flat-vector reference model. Price selection
// rescans every live order; it shares no book/index structures with the engine.
TEST_CASE("Randomized order flow agrees with independent reference matcher") {
    Engine e;
    std::mt19937 rng(4271);
    std::vector<Order> live;
    for (OrderID id = 1; id <= 5000; ++id) {
        if (id % 7 == 0 && !live.empty()) {
            const std::size_t index = rng() % live.size();
            REQUIRE(e.cancel_order(live[index].order_id));
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
        }
        auto o = order(id, rng() % 2 ? Side::BUY : Side::SELL,
                       95 + rng() % 11, 1 + rng() % 20, 1 + rng() % 8,
                       static_cast<OrderType>(rng() % 4));
        o.remaining_quantity = o.quantity;
        const auto eligible = [&](const Order& r) {
            return r.side != o.side && r.trader_id != o.trader_id &&
                (o.type == OrderType::MARKET || (o.side == Side::BUY ? r.price <= o.price : r.price >= o.price));
        };
        Quantity available = 0;
        for (const auto& r : live) if (eligible(r)) available += r.remaining_quantity;
        const bool rejected = o.type == OrderType::FOK && available < o.quantity;
        std::vector<Trade> expected;
        while (!rejected && o.remaining_quantity > 0) {
            auto best = live.end();
            for (auto it = live.begin(); it != live.end(); ++it) {
                if (!eligible(*it)) continue;
                if (best == live.end() || (o.side == Side::BUY ? it->price < best->price : it->price > best->price)) best = it;
            }
            if (best == live.end()) break;
            const auto qty = std::min(best->remaining_quantity, o.remaining_quantity);
            Trade t; t.price = best->price; t.quantity = qty; t.resting_order_id = best->order_id;
            expected.push_back(t);
            best->remaining_quantity -= qty; o.remaining_quantity -= qty;
            if (best->remaining_quantity == 0) live.erase(best);
        }
        const auto actual = e.add_order(o);
        REQUIRE(actual.size() == expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            REQUIRE(actual[i].resting_order_id == expected[i].resting_order_id);
            REQUIRE(actual[i].quantity == expected[i].quantity);
            REQUIRE(actual[i].price == expected[i].price);
        }
        const auto status = rejected ? OrderStatus::REJECTED : o.remaining_quantity == 0 ? OrderStatus::FILLED :
            o.type != OrderType::LIMIT ? OrderStatus::CANCELLED :
            o.remaining_quantity == o.quantity ? OrderStatus::NEW : OrderStatus::PARTIALLY_FILLED;
        REQUIRE(e.get_order_status(id) == status);
        if (!rejected && o.remaining_quantity && o.type == OrderType::LIMIT) live.push_back(o);
        REQUIRE(e.resting_order_count() == live.size());
        const auto snap = e.get_book_snapshot(live.size() + 1);
        Quantity reference_qty = 0, actual_qty = 0;
        for (const auto& r : live) reference_qty += r.remaining_quantity;
        for (const auto& l : snap.bids) actual_qty += l.quantity;
        for (const auto& l : snap.asks) actual_qty += l.quantity;
        REQUIRE(actual_qty == reference_qty);
        const auto top = e.get_top_of_book();
        REQUIRE(top.best_bid == (snap.bids.empty() ? std::optional<double>{} : snap.bids[0].price));
        REQUIRE(top.best_ask == (snap.asks.empty() ? std::optional<double>{} : snap.asks[0].price));
    }
}
