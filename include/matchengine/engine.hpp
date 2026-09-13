#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace matchengine {

using OrderID = std::uint64_t;
using TraderID = std::uint64_t;
using Quantity = std::uint64_t;
using Timestamp = std::uint64_t;

enum class Side { BUY, SELL };
enum class OrderType { LIMIT, MARKET, IOC, FOK };
enum class OrderStatus { NEW, PARTIALLY_FILLED, FILLED, CANCELLED, REJECTED };

struct Order {
    OrderID order_id{};
    TraderID trader_id{};
    Side side{Side::BUY};
    double price{};
    Quantity quantity{};
    Quantity remaining_quantity{};
    Timestamp timestamp{};
    OrderStatus status{OrderStatus::NEW};
    OrderType type{OrderType::LIMIT};
};

struct Trade {
    std::uint64_t trade_id{};
    double price{};
    Quantity quantity{};
    Timestamp timestamp{};
    OrderID aggressor_order_id{};
    OrderID resting_order_id{};
    Side aggressor_side{Side::BUY};
};

struct TopOfBook {
    std::optional<double> best_bid;
    std::optional<double> best_ask;
    Quantity bid_qty{};
    Quantity ask_qty{};
};

struct LevelSnapshot {
    double price{};
    Quantity quantity{};
    std::size_t order_count{};
};

struct BookSnapshot {
    std::vector<LevelSnapshot> bids;
    std::vector<LevelSnapshot> asks;
};

// Single-threaded. Arrival order, not caller-supplied timestamps, establishes FIFO.
class Engine {
public:
    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    std::vector<Trade> add_order(Order order);
    bool cancel_order(OrderID id);
    // new_qty is the replacement's remaining quantity; all replacements lose FIFO.
    std::vector<Trade> modify_order(OrderID id, double new_price, Quantity new_qty);
    OrderStatus get_order_status(OrderID id) const;
    TopOfBook get_top_of_book() const noexcept { return top_; }
    BookSnapshot get_book_snapshot(std::size_t depth) const;
    std::size_t resting_order_count() const noexcept { return locations_.size(); }

private:
    struct PriceLevel {
        std::list<Order> orders;
        Quantity quantity{};
    };
    using Bids = std::map<double, PriceLevel, std::greater<double>>;
    using Asks = std::map<double, PriceLevel>;
    struct OrderLocation {
        Side side;
        double price;
        std::list<Order>::iterator position;
    };
    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderID, OrderLocation> locations_;
    std::unordered_map<OrderID, OrderStatus> statuses_;
    TopOfBook top_;
    std::uint64_t next_trade_id_{1};

    static bool valid(const Order& order) noexcept;
    static bool crosses(const Order& order, double resting_price) noexcept;
    static Timestamp now();
    template <class Book> bool can_fill(const Book& book, const Order& order) const;
    template <class Book> void match(Book& book, Order& order, std::vector<Trade>& trades);
    template <class Book> void rest(Book& book, const Order& order);
    template <class Book> void remove(Book& book, const OrderLocation& location);
    void refresh_top() noexcept;
};

} // namespace matchengine
