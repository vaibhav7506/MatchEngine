#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace exchange {

using OrderReference = std::uint64_t;
using Shares = std::uint64_t;
// Exact units of 0.0001 currency. Do not convert to double while reconstructing.
using Price = std::uint32_t;
inline double decimal_price(Price price) noexcept { return static_cast<double>(price) / 10000.0; }
enum class Side { Buy, Sell };
enum class SessionState { MessagesStart, SystemOpen, MarketOpen, MarketClose, SystemClose, MessagesEnd };

struct Metadata {
    std::uint32_t instrument_id{};
    std::uint16_t tracking_number{};
    std::uint64_t timestamp_ns{}; // session-relative nanoseconds since midnight
};
struct SystemEvent { SessionState state{}; };
struct InstrumentDirectory {
    std::string symbol;
    char market_category{}, financial_status{};
    std::uint32_t round_lot_size{};
    char round_lots_only{}, issue_classification{};
    std::string issue_subtype;
    char authenticity{}, short_sale_threshold{}, ipo_flag{}, price_tier{}, etp_flag{};
    std::uint32_t leverage_factor{};
    char inverse_indicator{};
};
struct OrderAdded {
    OrderReference reference{};
    Side side{};
    Shares quantity{};
    std::string symbol;
    Price price{};
    std::optional<std::string> participant;
};
struct OrderExecuted {
    OrderReference reference{};
    Shares quantity{};
    std::uint64_t match_id{};
    bool printable{true};
    std::optional<Price> execution_price; // absent: use resting order's price
};
struct OrderCancelled { OrderReference reference{}; Shares quantity{}; };
struct OrderDeleted { OrderReference reference{}; };
struct OrderReplaced { OrderReference reference{}, new_reference{}; Shares quantity{}; Price price{}; };
struct Trade {
    Shares quantity{};
    std::string symbol;
    Price price{};
    std::uint64_t match_id{};
    // Non-displayed trade: no resting reference or inferred buy/sell direction.
};
struct Administrative {
    // Known, length-validated non-book message. Payload semantics are out of scope.
    std::string name;
};
using Payload = std::variant<SystemEvent, InstrumentDirectory, OrderAdded, OrderExecuted,
    OrderCancelled, OrderDeleted, OrderReplaced, Trade, Administrative>;
struct Event { Metadata metadata; Payload payload; };

} // namespace exchange
