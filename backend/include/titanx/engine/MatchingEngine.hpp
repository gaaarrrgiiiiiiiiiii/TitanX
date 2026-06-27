#pragma once

#include "titanx/engine/OrderBook.hpp"
#include "titanx/event/OrderEvent.hpp"
#include "titanx/event/FillEvent.hpp"
#include "titanx/risk/RiskEngine.hpp"

#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace titanx {

/**
 * ============================================================
 * TitanX MatchingEngine — Orchestrator (C++17)
 * ============================================================
 *
 * Concurrency Model:
 *   Per-symbol std::shared_mutex — different instruments match
 *   concurrently. All order operations that modify the book acquire
 *   the EXCLUSIVE lock; read queries (market data) use SHARED lock
 *   allowing concurrent reads from multiple WebSocket subscribers.
 *
 *   The next evolution is a Disruptor-based ring buffer per symbol —
 *   that eliminates the lock entirely via single-threaded ring
 *   consumption, achieving sub-microsecond latency.
 *
 * Global kill switch:
 *   std::atomic<bool> — readable on the hot path without any
 *   synchronization overhead. A single atomic load instruction.
 */
class MatchingEngine {
public:
    explicit MatchingEngine(RiskEngine& riskEngine);

    // ---- Order submission (main entry point) ----
    std::vector<Trade> submit(const Order& order);

    // ---- Order cancellation ----
    bool cancel(const std::string& symbol, const std::string& orderId);

    // ---- Read queries (uses shared/read lock) ----
    std::optional<OrderBookSnapshot> getSnapshot(const std::string& symbol, int depth);

    // ---- Control ----
    void halt();
    void resume();
    bool isHalted() const { return halted_.load(std::memory_order_relaxed); }

    // ---- Event callbacks (dependency inversion) ----
    void setEventConsumer(std::function<void(const OrderEvent&)> consumer);
    void setFillConsumer(std::function<void(const FillEvent&)> consumer);

    // ---- Accessors ----
    const std::unordered_map<std::string, std::unique_ptr<OrderBook>>&
        getBooks() const { return books_; }

private:
    // One OrderBook per symbol
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;

    // One mutex per symbol — instruments match independently
    std::unordered_map<std::string, std::unique_ptr<std::shared_mutex>> locks_;

    // Stop-limit pending orders: symbol → list of pending stop orders
    std::unordered_map<std::string, std::vector<Order>> stopOrders_;

    // Mutex for map-level operations (adding new symbols)
    mutable std::mutex mapMutex_;

    RiskEngine& riskEngine_;

    std::function<void(const OrderEvent&)> eventConsumer_ = [](const OrderEvent&){};
    std::function<void(const FillEvent&)>  fillConsumer_  = [](const FillEvent&){};

    // Global kill switch
    std::atomic<bool> halted_{false};

    // ---- Internal ----
    OrderBook& getOrCreateBook(const std::string& symbol);
    std::shared_mutex& getOrCreateLock(const std::string& symbol);
    void checkStopTriggers(const std::string& symbol, OrderBook& book);
    void emit(const OrderEvent& event);
};

} // namespace titanx
