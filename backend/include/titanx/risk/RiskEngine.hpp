#pragma once

#include "titanx/model/Order.hpp"
#include "titanx/risk/RiskResult.hpp"
#include "titanx/risk/CircuitBreaker.hpp"

#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <string>

namespace titanx {

/**
 * ============================================================
 * TitanX RiskEngine — Pre-Trade Risk Controls (C++17)
 * ============================================================
 *
 * All 6 pre-trade checks from the Java version:
 *  1. Circuit breaker halt check (per-symbol)
 *  2. Duplicate order ID (idempotency)
 *  3. Notional value limit ($5M per order)
 *  4. Position limit (10K shares net long/short)
 *  5. Fat-finger protection (10% deviation from last trade)
 *  6. Self-trade prevention (same account both sides)
 */
class RiskEngine {
public:
    explicit RiskEngine(CircuitBreaker& circuitBreaker);

    // ---- Main entry point ----
    RiskResult checkPreTrade(const Order& order);

    // ---- Post-trade updates (called by MatchingEngine after fills) ----
    void recordFill(const std::string& accountId, const std::string& symbol,
                    OrderSide side, int64_t quantity);
    void updateLastTradePrice(const std::string& symbol, int64_t price);

    // ---- Queries ----
    int64_t getNetPosition(const std::string& accountId,
                           const std::string& symbol) const;
    uint64_t totalChecked()  const { return totalChecked_.load(std::memory_order_relaxed); }
    uint64_t totalRejected() const { return totalRejected_.load(std::memory_order_relaxed); }

private:
    // Configuration
    static constexpr int64_t MAX_POSITION_SHARES = 10'000;
    static constexpr int64_t MAX_NOTIONAL_FIXED  = 5'000'000LL * PRICE_SCALE; // $5M
    static constexpr int64_t FAT_FINGER_BPS      = 1000; // 10% = 1000 basis points

    // State (all guarded by mutex_)
    // accountId → symbol → net position
    std::unordered_map<std::string,
        std::unordered_map<std::string, int64_t>> positions_;

    // accountId → set of active order IDs on buy/sell side
    std::unordered_map<std::string, std::unordered_set<std::string>> activeBuyOrders_;
    std::unordered_map<std::string, std::unordered_set<std::string>> activeSellOrders_;

    // Seen order IDs for idempotency
    std::unordered_set<std::string> seenOrderIds_;

    // Last trade prices per symbol
    std::unordered_map<std::string, int64_t> lastTradePrices_;

    // Counters
    std::atomic<uint64_t> totalChecked_{0};
    std::atomic<uint64_t> totalRejected_{0};

    CircuitBreaker& circuitBreaker_;
    mutable std::mutex mutex_;

    void trackOrder(const Order& order);
    RiskResult reject(const std::string& reason);
};

} // namespace titanx
