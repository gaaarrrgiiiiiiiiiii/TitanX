#include "titanx/event/OrderEvent.hpp"

namespace titanx {

OrderEvent OrderEvent::received(const Order& order) {
    return build(EventType::ORDER_RECEIVED, order, std::nullopt, "", 0);
}

OrderEvent OrderEvent::resting(const Order& order) {
    return build(EventType::ORDER_RESTING, order, std::nullopt, "", 0);
}

OrderEvent OrderEvent::filled(const Order& order, const Trade& trade) {
    return build(EventType::ORDER_FILLED, order, trade, "", 0);
}

OrderEvent OrderEvent::partiallyFilled(const Order& order, const Trade& trade) {
    return build(EventType::ORDER_PARTIALLY_FILLED, order, trade, "", 0);
}

OrderEvent OrderEvent::cancelled(const Order& order) {
    return build(EventType::ORDER_CANCELLED, order, std::nullopt, "", 0);
}

OrderEvent OrderEvent::rejected(const Order& order, const std::string& reason) {
    return build(EventType::ORDER_REJECTED, order, std::nullopt, reason, 0);
}

OrderEvent OrderEvent::build(EventType type, const Order& order,
                             const std::optional<Trade>& trade,
                             const std::string& reason,
                             long latencyMicros) {
    return OrderEvent{
        generate_uuid(),
        type,
        order.orderId,
        order.accountId,
        order.symbol,
        order,
        trade,
        reason,
        latencyMicros,
        SystemClock::now()
    };
}

nlohmann::json OrderEvent::to_json() const {
    nlohmann::json j;
    j["eventId"]         = eventId;
    j["type"]            = titanx::to_string(type);
    j["orderId"]         = orderId;
    j["accountId"]       = accountId;
    j["symbol"]          = symbol;
    j["orderSnapshot"]   = orderSnapshot.to_json();
    j["trade"]           = trade ? trade->to_json() : nullptr;
    j["rejectionReason"] = rejectionReason.empty() ? nullptr
                           : nlohmann::json(rejectionReason);
    j["latencyMicros"]   = latencyMicros;

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        occurredAt.time_since_epoch()).count();
    j["occurredAt"] = ms;
    return j;
}

} // namespace titanx
