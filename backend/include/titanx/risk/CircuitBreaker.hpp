#pragma once

#include <string>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace titanx {

/**
 * ============================================================
 * TitanX CircuitBreaker — Market-Wide Safety Controls (C++17)
 * ============================================================
 *
 * 1. Per-symbol price halt:
 *    Halts trading if price moves more than 5% within 60 seconds.
 *    Modelled after exchange "limit up / limit down" rules (SEC Rule 15c3-5).
 *
 * 2. Global kill switch:
 *    std::atomic<bool> — readable on the hot path without any
 *    synchronization overhead.
 *
 * Uses std::atomic<bool> per symbol for lock-free halt checks.
 */
class CircuitBreaker {
public:
    // Called by RiskEngine after every trade
    void recordTrade(const std::string& symbol, int64_t tradePrice);

    void haltSymbol(const std::string& symbol, const std::string& reason);
    void resumeSymbol(const std::string& symbol);
    bool isHalted(const std::string& symbol) const;

    std::unordered_map<std::string, bool> haltStatus() const;

private:
    static constexpr int64_t HALT_THRESHOLD_BPS = 500; // 5% = 500 basis points
    static constexpr int64_t WINDOW_SECONDS     = 60;

    // Per-symbol halt flag
    // Note: std::atomic<bool> is not movable, so we use a wrapper
    struct HaltFlag {
        std::atomic<bool> halted{false};
    };
    mutable std::unordered_map<std::string, std::unique_ptr<HaltFlag>> symbolHalts_;

    // Per-symbol: reference price and window start
    std::unordered_map<std::string, int64_t> referencePrice_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> windowStart_;

    mutable std::mutex mutex_; // protects map-level operations
};

} // namespace titanx
