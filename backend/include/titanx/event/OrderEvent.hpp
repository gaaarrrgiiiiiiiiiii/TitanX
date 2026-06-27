#pragma once

#include "titanx/event/EventType.hpp"
#include "titanx/model/Order.hpp"
#include "titanx/model/Trade.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <chrono>

namespace titanx {

/**
 * ============================================================
 * TitanX OrderEvent — Event Sourcing Atom (C++17)
 * ============================================================
 *
 * Every state change produces an immutable OrderEvent appended to
 * the order_events table in PostgreSQL.
 *
 * Why event sourcing?
 *   1. Crash recovery — replay events to reconstruct book state
 *   2. Full audit trail — MiFID II and RegNMS compliant
 *   3. Time-travel queries — "what was the order book at 14:32:07?"
 */
struct OrderEvent {
    std::string           eventId;
    EventType             type;
    std::string           orderId;
    std::string           accountId;
    std::string           symbol;
    Order                 orderSnapshot;
    std::optional<Trade>  trade;           // non-null for FILLED events
    std::string           rejectionReason; // non-empty for REJECTED events
    long                  latencyMicros;
    SysTimePoint          occurredAt;

    // ---- Factory methods ----
    static OrderEvent received(const Order& order);
    static OrderEvent resting(const Order& order);
    static OrderEvent filled(const Order& order, const Trade& trade);
    static OrderEvent partiallyFilled(const Order& order, const Trade& trade);
    static OrderEvent cancelled(const Order& order);
    static OrderEvent rejected(const Order& order, const std::string& reason);

    nlohmann::json to_json() const;

private:
    static OrderEvent build(EventType type, const Order& order,
                            const std::optional<Trade>& trade,
                            const std::string& reason,
                            long latencyMicros);
};

} // namespace titanx
