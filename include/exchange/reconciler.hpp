#pragma once

#include "exchange/events.hpp"
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exchange {

struct AuditRow {
    OrderReference root_reference{}, current_reference{};
    Shares added{}, replacement_added{}, executed{}, cancelled{}, deleted{}, replaced_out{}, remaining{};
    bool balanced() const noexcept;
};

// First N add events form a deterministic reference sample. Every subsequent
// event for those references and every replacement descendant is checked.
// Sampling is explicit: unsampled references are not claimed to be reconciled.
class Reconciler {
public:
    explicit Reconciler(std::size_t sample_limit = 10000) : sample_limit_(sample_limit) {}
    void apply(const Event& event);
    void verify() const;
    const std::vector<AuditRow>& audit() const noexcept { return audit_; }
    std::size_t live_orders() const noexcept { return live_.size(); }
    std::uint64_t checked_updates() const noexcept { return checked_updates_; }
    Shares remaining_quantity() const;
private:
    struct LiveOrder {
        std::size_t audit_index;
        std::uint32_t instrument;
        Side side;
        Price price;
        Shares remaining;
    };
    std::size_t sample_limit_;
    std::vector<AuditRow> audit_;
    std::unordered_map<OrderReference, LiveOrder> live_;
    std::unordered_set<OrderReference> tracked_; // includes closed refs to catch late updates/reuse
    std::uint64_t checked_updates_{};
    void add(const Metadata&, const OrderAdded&);
    void reduce(const Metadata&, OrderReference, Shares, char kind);
    void replace(const Metadata&, const OrderReplaced&);
};

} // namespace exchange
