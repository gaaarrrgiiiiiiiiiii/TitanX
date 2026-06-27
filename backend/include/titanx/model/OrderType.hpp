#pragma once

#include <string>
#include <stdexcept>

namespace titanx {

/**
 * Supported order types.
 *
 * MARKET     — execute immediately at best available price
 * LIMIT      — execute at specified price or better; rest in book if not matched
 * IOC        — Immediate-or-Cancel: fill what's available, cancel the rest
 * FOK        — Fill-or-Kill: fill completely or cancel entirely (no partial fills)
 * STOP_LIMIT — becomes a LIMIT order when stopPrice is touched
 */
enum class OrderType {
    MARKET,
    LIMIT,
    IOC,
    FOK,
    STOP_LIMIT
};

inline std::string to_string(OrderType type) {
    switch (type) {
        case OrderType::MARKET:     return "MARKET";
        case OrderType::LIMIT:      return "LIMIT";
        case OrderType::IOC:        return "IOC";
        case OrderType::FOK:        return "FOK";
        case OrderType::STOP_LIMIT: return "STOP_LIMIT";
    }
    return "UNKNOWN";
}

inline OrderType order_type_from_string(const std::string& s) {
    if (s == "MARKET")     return OrderType::MARKET;
    if (s == "LIMIT")      return OrderType::LIMIT;
    if (s == "IOC")        return OrderType::IOC;
    if (s == "FOK")        return OrderType::FOK;
    if (s == "STOP_LIMIT") return OrderType::STOP_LIMIT;
    throw std::invalid_argument("Invalid OrderType: " + s);
}

} // namespace titanx
