#include "exchange/reconciler.hpp"

#include <limits>
#include <string>
#include <type_traits>

namespace exchange {
namespace {
void increment(Shares& counter, Shares quantity) {
    if (counter > std::numeric_limits<Shares>::max() - quantity)
        throw std::overflow_error("Reconciliation quantity overflow");
    counter += quantity;
}
[[noreturn]] void fail(OrderReference reference, const char* message) {
    throw std::runtime_error("Reconciliation order " + std::to_string(reference) + ": " + message);
}
} // namespace

bool AuditRow::balanced() const noexcept {
    // Subtract instead of summing reductions; never permit unsigned underflow.
    if (added > std::numeric_limits<Shares>::max() - replacement_added) return false;
    auto available = added + replacement_added;
    for (auto removed : {executed, cancelled, deleted, replaced_out}) {
        if (removed > available) return false;
        available -= removed;
    }
    return available == remaining;
}

void Reconciler::add(const Metadata& meta, const OrderAdded& order) {
    if (tracked_.count(order.reference)) fail(order.reference, "duplicate tracked reference");
    if (audit_.size() >= sample_limit_) return;
    if (order.reference == 0 || order.quantity == 0) fail(order.reference, "invalid add reference/quantity");
    const auto index = audit_.size();
    audit_.push_back({order.reference, order.reference, order.quantity, 0, 0, 0, 0, 0, order.quantity});
    tracked_.insert(order.reference);
    live_.emplace(order.reference, LiveOrder{index, meta.instrument_id, order.side, order.price, order.quantity});
}

void Reconciler::reduce(const Metadata& meta, OrderReference reference, Shares quantity, char kind) {
    if (!tracked_.count(reference)) return; // explicitly outside selected sample
    auto it = live_.find(reference);
    if (it == live_.end()) fail(reference, "update for closed/replaced reference");
    auto& order = it->second;
    if (meta.instrument_id != order.instrument) fail(reference, "instrument changed");
    if (kind == 'D') quantity = order.remaining;
    if (quantity == 0 || quantity > order.remaining) fail(reference, "zero or excessive reduction");
    auto& audit = audit_[order.audit_index];
    if (kind == 'E') increment(audit.executed, quantity);
    else if (kind == 'X') increment(audit.cancelled, quantity);
    else increment(audit.deleted, quantity);
    order.remaining -= quantity;
    audit.remaining -= quantity;
    ++checked_updates_;
    if (!audit.balanced()) fail(reference, "ledger quantity imbalance");
    if (order.remaining == 0) live_.erase(it);
}

void Reconciler::replace(const Metadata& meta, const OrderReplaced& replacement) {
    if (tracked_.count(replacement.new_reference)) fail(replacement.new_reference, "replacement reuses tracked reference");
    if (!tracked_.count(replacement.reference)) return;
    auto it = live_.find(replacement.reference);
    if (it == live_.end()) fail(replacement.reference, "replace for closed/replaced reference");
    if (meta.instrument_id != it->second.instrument) fail(replacement.reference, "replacement instrument changed");
    if (replacement.new_reference == 0 || replacement.quantity == 0)
        fail(replacement.reference, "invalid replacement reference/quantity");
    auto next = it->second; // side and instrument are inherited, not guessed
    auto& audit = audit_[next.audit_index];
    increment(audit.replaced_out, next.remaining);
    increment(audit.replacement_added, replacement.quantity);
    audit.current_reference = replacement.new_reference;
    audit.remaining = replacement.quantity;
    next.remaining = replacement.quantity;
    next.price = replacement.price;
    live_.erase(it);
    live_.emplace(replacement.new_reference, next);
    tracked_.insert(replacement.new_reference);
    ++checked_updates_;
    if (!audit.balanced()) fail(replacement.reference, "replacement ledger imbalance");
}

void Reconciler::apply(const Event& event) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, OrderAdded>) add(event.metadata, value);
        else if constexpr (std::is_same_v<T, OrderExecuted>) reduce(event.metadata, value.reference, value.quantity, 'E');
        else if constexpr (std::is_same_v<T, OrderCancelled>) reduce(event.metadata, value.reference, value.quantity, 'X');
        else if constexpr (std::is_same_v<T, OrderDeleted>) reduce(event.metadata, value.reference, 0, 'D');
        else if constexpr (std::is_same_v<T, OrderReplaced>) replace(event.metadata, value);
        // P trades, Q crosses and B breaks never alter displayed resting quantity.
    }, event.payload);
}

void Reconciler::verify() const {
    Shares ledger_remaining = 0;
    for (const auto& row : audit_) {
        if (!row.balanced()) fail(row.root_reference, "unbalanced audit row");
        increment(ledger_remaining, row.remaining);
    }
    Shares book_remaining = 0;
    for (const auto& [reference, order] : live_) {
        const auto& row = audit_[order.audit_index];
        if (row.current_reference != reference || row.remaining != order.remaining)
            fail(reference, "ledger and live order disagree");
        increment(book_remaining, order.remaining);
    }
    if (book_remaining != ledger_remaining) throw std::runtime_error("Aggregate ledger/live book mismatch");
}

Shares Reconciler::remaining_quantity() const {
    Shares quantity = 0;
    for (const auto& [reference, order] : live_) { (void)reference; increment(quantity, order.remaining); }
    return quantity;
}

} // namespace exchange
