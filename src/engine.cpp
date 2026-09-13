#include "matchengine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace matchengine {

Timestamp Engine::now() {
    return static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

bool Engine::valid(const Order& o) noexcept {
    if (o.side != Side::BUY && o.side != Side::SELL) return false;
    if (o.type != OrderType::LIMIT && o.type != OrderType::MARKET &&
        o.type != OrderType::IOC && o.type != OrderType::FOK) return false;
    // Bounded individual quantities leave ample headroom for level aggregates.
    return o.order_id != 0 && o.quantity > 0 &&
        o.quantity <= std::numeric_limits<std::uint32_t>::max() &&
        std::isfinite(o.price) && (o.type == OrderType::MARKET || o.price > 0);
}

bool Engine::crosses(const Order& o, double price) noexcept {
    return o.type == OrderType::MARKET ||
        (o.side == Side::BUY ? price <= o.price : price >= o.price);
}

template <class Book>
bool Engine::can_fill(const Book& book, const Order& o) const {
    Quantity needed = o.remaining_quantity;
    for (const auto& [price, level] : book) {
        if (!crosses(o, price)) break;
        for (const auto& resting : level.orders) {
            if (resting.trader_id == o.trader_id) continue;
            if (resting.remaining_quantity >= needed) return true;
            needed -= resting.remaining_quantity;
        }
    }
    return false;
}

template <class Book>
void Engine::match(Book& book, Order& o, std::vector<Trade>& trades) {
    auto level_it = book.begin();
    while (level_it != book.end() && o.remaining_quantity > 0) {
        if (!crosses(o, level_it->first)) break;
        auto& level = level_it->second;
        auto it = level.orders.begin();
        while (it != level.orders.end() && o.remaining_quantity > 0) {
            if (it->trader_id == o.trader_id) { ++it; continue; }
            const Quantity quantity = std::min(o.remaining_quantity, it->remaining_quantity);
            trades.push_back({next_trade_id_++, level_it->first, quantity, now(),
                              o.order_id, it->order_id, o.side});
            o.remaining_quantity -= quantity;
            it->remaining_quantity -= quantity;
            level.quantity -= quantity;
            it->status = it->remaining_quantity == 0 ? OrderStatus::FILLED : OrderStatus::PARTIALLY_FILLED;
            statuses_.at(it->order_id) = it->status;
            if (it->remaining_quantity == 0) {
                locations_.erase(it->order_id);
                it = level.orders.erase(it);
            } else {
                ++it;
            }
        }
        if (level.orders.empty()) level_it = book.erase(level_it);
        else ++level_it;
    }
}

template <class Book>
void Engine::rest(Book& book, const Order& o) {
    auto& level = book[o.price];
    if (level.quantity > std::numeric_limits<Quantity>::max() - o.remaining_quantity)
        throw std::overflow_error("Price level quantity overflow");
    level.orders.push_back(o);
    locations_.emplace(o.order_id, OrderLocation{o.side, o.price, std::prev(level.orders.end())});
    level.quantity += o.remaining_quantity;
}

std::vector<Trade> Engine::add_order(Order o) {
    if (statuses_.find(o.order_id) != statuses_.end())
        throw std::invalid_argument("Order ID has already been used");
    statuses_.emplace(o.order_id, OrderStatus::REJECTED);
    if (!valid(o)) return {};
    o.remaining_quantity = o.quantity;
    o.status = OrderStatus::NEW;
    if (o.timestamp == 0) o.timestamp = now();
    if (o.type == OrderType::FOK &&
        !(o.side == Side::BUY ? can_fill(asks_, o) : can_fill(bids_, o))) return {};

    std::vector<Trade> trades;
    if (o.side == Side::BUY) match(asks_, o, trades);
    else match(bids_, o, trades);

    if (o.remaining_quantity == 0) o.status = OrderStatus::FILLED;
    else if (o.type == OrderType::LIMIT) {
        o.status = o.remaining_quantity == o.quantity ? OrderStatus::NEW : OrderStatus::PARTIALLY_FILLED;
        if (o.side == Side::BUY) rest(bids_, o);
        else rest(asks_, o);
    } else o.status = OrderStatus::CANCELLED;
    statuses_.at(o.order_id) = o.status;
    refresh_top();
    return trades;
}

template <class Book>
void Engine::remove(Book& book, const OrderLocation& location) {
    const auto it = book.find(location.price);
    auto& level = it->second;
    level.quantity -= location.position->remaining_quantity;
    level.orders.erase(location.position);
    if (level.orders.empty()) book.erase(it);
}

bool Engine::cancel_order(OrderID id) {
    const auto it = locations_.find(id);
    if (it == locations_.end()) return false;
    if (it->second.side == Side::BUY) remove(bids_, it->second);
    else remove(asks_, it->second);
    locations_.erase(it);
    statuses_.at(id) = OrderStatus::CANCELLED;
    refresh_top();
    return true;
}

std::vector<Trade> Engine::modify_order(OrderID id, double price, Quantity qty) {
    const auto it = locations_.find(id);
    if (it == locations_.end()) throw std::out_of_range("Order is not resting");
    Order replacement = *it->second.position;
    replacement.price = price;
    replacement.quantity = qty;
    replacement.timestamp = now();
    if (!valid(replacement)) throw std::invalid_argument("Invalid replacement price or quantity");
    cancel_order(id);
    statuses_.erase(id);
    return add_order(replacement);
}

OrderStatus Engine::get_order_status(OrderID id) const {
    return statuses_.at(id);
}

void Engine::refresh_top() noexcept {
    top_ = {};
    if (!bids_.empty()) {
        top_.best_bid = bids_.begin()->first;
        top_.bid_qty = bids_.begin()->second.quantity;
    }
    if (!asks_.empty()) {
        top_.best_ask = asks_.begin()->first;
        top_.ask_qty = asks_.begin()->second.quantity;
    }
}

BookSnapshot Engine::get_book_snapshot(std::size_t depth) const {
    BookSnapshot result;
    const auto copy = [depth](const auto& book, auto& out) {
        out.reserve(std::min(depth, book.size()));
        for (auto it = book.begin(); it != book.end() && out.size() < depth; ++it)
            out.push_back({it->first, it->second.quantity, it->second.orders.size()});
    };
    copy(bids_, result.bids);
    copy(asks_, result.asks);
    return result;
}

} // namespace matchengine
