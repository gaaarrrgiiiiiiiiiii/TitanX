#include "titanx/risk/CircuitBreaker.hpp"
#include <spdlog/spdlog.h>
#include <cmath>

namespace titanx {

void CircuitBreaker::recordTrade(const std::string& symbol, int64_t tradePrice) {
    std::lock_guard<std::mutex> guard(mutex_);

    auto now = std::chrono::steady_clock::now();

    auto refIt = referencePrice_.find(symbol);
    auto winIt = windowStart_.find(symbol);

    if (refIt == referencePrice_.end() || winIt == windowStart_.end()) {
        // Start a new window
        referencePrice_[symbol] = tradePrice;
        windowStart_[symbol] = now;
        return;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - winIt->second).count();

    if (elapsed > WINDOW_SECONDS) {
        // Start a new window
        refIt->second = tradePrice;
        winIt->second = now;
        return;
    }

    // Calculate % move from window start (in basis points for integer math)
    int64_t refPrice = refIt->second;
    if (refPrice == 0) return;

    int64_t absDiff = std::abs(tradePrice - refPrice);
    int64_t pctMoveBps = (absDiff * 10000) / refPrice; // basis points

    if (pctMoveBps > HALT_THRESHOLD_BPS) {
        double pctMove = static_cast<double>(pctMoveBps) / 100.0;
        std::string reason = "Price moved " + std::to_string(pctMove) +
            "% in " + std::to_string(WINDOW_SECONDS) + "s";
        haltSymbol(symbol, reason);
    }
}

void CircuitBreaker::haltSymbol(const std::string& symbol, const std::string& reason) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = symbolHalts_.find(symbol);
        if (it == symbolHalts_.end()) {
            symbolHalts_[symbol] = std::make_unique<HaltFlag>();
        }
        symbolHalts_[symbol]->halted.store(true, std::memory_order_relaxed);
    }
    spdlog::warn("CIRCUIT BREAKER TRIGGERED on {} — {}", symbol, reason);
}

void CircuitBreaker::resumeSymbol(const std::string& symbol) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = symbolHalts_.find(symbol);
    if (it != symbolHalts_.end()) {
        it->second->halted.store(false, std::memory_order_relaxed);
    }
    referencePrice_.erase(symbol);
    windowStart_.erase(symbol);
    spdlog::info("Circuit breaker cleared for {}", symbol);
}

bool CircuitBreaker::isHalted(const std::string& symbol) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = symbolHalts_.find(symbol);
    if (it == symbolHalts_.end()) return false;
    return it->second->halted.load(std::memory_order_relaxed);
}

std::unordered_map<std::string, bool> CircuitBreaker::haltStatus() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::unordered_map<std::string, bool> status;
    for (const auto& [sym, flag] : symbolHalts_) {
        status[sym] = flag->halted.load(std::memory_order_relaxed);
    }
    return status;
}

} // namespace titanx
