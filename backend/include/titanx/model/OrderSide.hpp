#pragma once

#include <string>
#include <stdexcept>

namespace titanx {

enum class OrderSide {
    BUY,
    SELL
};

inline std::string to_string(OrderSide side) {
    switch (side) {
        case OrderSide::BUY:  return "BUY";
        case OrderSide::SELL: return "SELL";
    }
    return "UNKNOWN";
}

inline OrderSide order_side_from_string(const std::string& s) {
    if (s == "BUY")  return OrderSide::BUY;
    if (s == "SELL") return OrderSide::SELL;
    throw std::invalid_argument("Invalid OrderSide: " + s);
}

} // namespace titanx
