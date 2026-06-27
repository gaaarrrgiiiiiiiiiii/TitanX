#pragma once

#include <string>

namespace titanx {

/**
 * Event types mirror FIX protocol ExecutionReport ExecType values.
 */
enum class EventType {
    ORDER_RECEIVED,
    ORDER_RESTING,
    ORDER_PARTIALLY_FILLED,
    ORDER_FILLED,
    ORDER_CANCELLED,
    ORDER_REJECTED,
    CIRCUIT_BREAKER_TRIGGERED,
    ENGINE_HALTED,
    ENGINE_RESUMED
};

inline std::string to_string(EventType type) {
    switch (type) {
        case EventType::ORDER_RECEIVED:            return "ORDER_RECEIVED";
        case EventType::ORDER_RESTING:             return "ORDER_RESTING";
        case EventType::ORDER_PARTIALLY_FILLED:    return "ORDER_PARTIALLY_FILLED";
        case EventType::ORDER_FILLED:              return "ORDER_FILLED";
        case EventType::ORDER_CANCELLED:           return "ORDER_CANCELLED";
        case EventType::ORDER_REJECTED:            return "ORDER_REJECTED";
        case EventType::CIRCUIT_BREAKER_TRIGGERED: return "CIRCUIT_BREAKER_TRIGGERED";
        case EventType::ENGINE_HALTED:             return "ENGINE_HALTED";
        case EventType::ENGINE_RESUMED:            return "ENGINE_RESUMED";
    }
    return "UNKNOWN";
}

} // namespace titanx
