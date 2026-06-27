#pragma once

#include <string>

namespace titanx {

enum class OrderStatus {
    PENDING,
    PARTIALLY_FILLED,
    FILLED,
    CANCELLED,
    REJECTED
};

inline std::string to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::PENDING:          return "PENDING";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::CANCELLED:        return "CANCELLED";
        case OrderStatus::REJECTED:         return "REJECTED";
    }
    return "UNKNOWN";
}

} // namespace titanx
