#pragma once

#include "titanx/model/OrderType.hpp"
#include "titanx/model/OrderSide.hpp"
#include "titanx/model/OrderStatus.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <chrono>
#include <optional>

namespace titanx {

/**
 * ============================================================
 * TitanX Order — Core Domain Object (C++17)
 * ============================================================
 *
 * Key design decisions:
 *
 * 1. FIXED-POINT PRICES (int64_t, scale factor 10^8):
 *    $150.00 is stored as 15000000000.
 *    Integer arithmetic is 50-100x faster than BigDecimal.
 *    This is what every HFT firm uses — Citadel, Jane Street, LMAX.
 *
 * 2. VALUE SEMANTICS with copy-on-modify:
 *    withRemainingQuantity() returns a new Order copy.
 *    No shared mutable state → no data races.
 *
 * 3. FIFO ordering via createdAt for price-time priority.
 */

// Price scale: 1 unit = 10^-8 dollars (8 decimal places)
static constexpr int64_t PRICE_SCALE = 100'000'000LL;

// Convert a double price to fixed-point (for input parsing)
inline int64_t price_to_fixed(double price) {
    return static_cast<int64_t>(price * PRICE_SCALE + 0.5);
}

// Convert a string price to fixed-point
inline int64_t price_to_fixed(const std::string& price_str) {
    return price_to_fixed(std::stod(price_str));
}

// Convert fixed-point back to double (for output formatting)
inline double fixed_to_price(int64_t fixed) {
    return static_cast<double>(fixed) / PRICE_SCALE;
}

// Format fixed-point as string with 2 decimal places
inline std::string price_to_string(int64_t fixed) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", fixed_to_price(fixed));
    return buf;
}

using SteadyClock = std::chrono::steady_clock;
using TimePoint   = std::chrono::steady_clock::time_point;
using SystemClock = std::chrono::system_clock;
using SysTimePoint = std::chrono::system_clock::time_point;

struct Order {
    std::string   orderId;
    std::string   accountId;
    std::string   symbol;
    OrderSide     side;
    OrderType     type;
    int64_t       price;              // fixed-point, 0 for MARKET orders
    int64_t       stopPrice;          // fixed-point, 0 if not STOP_LIMIT
    int64_t       quantity;
    int64_t       remainingQuantity;
    OrderStatus   status;
    SysTimePoint  createdAt;
    SysTimePoint  updatedAt;

    // ---- Factory method ----
    static Order create(const std::string& accountId,
                        const std::string& symbol,
                        OrderSide side,
                        OrderType type,
                        int64_t price,
                        int64_t stopPrice,
                        int64_t quantity);

    // ---- Copy-on-modify (functional update) ----
    [[nodiscard]] Order withRemainingQuantity(int64_t newRemaining) const;
    [[nodiscard]] Order withStatus(OrderStatus newStatus) const;

    // ---- Convenience ----
    bool isBuy()    const { return side == OrderSide::BUY; }
    bool isSell()   const { return side == OrderSide::SELL; }
    bool isMarket() const { return type == OrderType::MARKET; }
    bool isActive() const {
        return status == OrderStatus::PENDING ||
               status == OrderStatus::PARTIALLY_FILLED;
    }

    // FIFO ordering: earlier orders have priority within a price level
    bool operator<(const Order& other) const {
        // For std::priority_queue (max-heap), we invert:
        // we want earliest first, so "less" = later timestamp
        return createdAt > other.createdAt;
    }

    // JSON serialization
    nlohmann::json to_json() const;
    std::string to_string() const;
};

// Generate a UUID-like string
std::string generate_uuid();

} // namespace titanx
