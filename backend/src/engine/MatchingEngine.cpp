#include "titanx/engine/MatchingEngine.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace titanx {

MatchingEngine::MatchingEngine(RiskEngine& riskEngine)
    : riskEngine_(riskEngine) {}

// ---------------------------------------------------------------
//  INTERNAL: Get or create book / lock for a symbol
// ---------------------------------------------------------------

OrderBook& MatchingEngine::getOrCreateBook(const std::string& symbol) {
    std::lock_guard<std::mutex> guard(mapMutex_);
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        auto [inserted, _] = books_.emplace(symbol, std::make_unique<OrderBook>(symbol));
        return *inserted->second;
    }
    return *it->second;
}

std::shared_mutex& MatchingEngine::getOrCreateLock(const std::string& symbol) {
    std::lock_guard<std::mutex> guard(mapMutex_);
    auto it = locks_.find(symbol);
    if (it == locks_.end()) {
        auto [inserted, _] = locks_.emplace(symbol, std::make_unique<std::shared_mutex>());
        return *inserted->second;
    }
    return *it->second;
}

// ---------------------------------------------------------------
//  ORDER SUBMISSION (main entry point)
// ---------------------------------------------------------------

std::vector<Trade> MatchingEngine::submit(const Order& order) {
    // 1. Global halt check
    if (halted_.load(std::memory_order_relaxed)) {
        spdlog::warn("Engine halted — rejecting order {}", order.orderId);
        emit(OrderEvent::rejected(order, "ENGINE_HALTED"));
        return {};
    }

    // 2. Pre-trade risk checks
    RiskResult riskResult = riskEngine_.checkPreTrade(order);
    if (!riskResult.approved) {
        spdlog::info("Order {} rejected by risk: {}", order.orderId, riskResult.reason);
        emit(OrderEvent::rejected(order, riskResult.reason));
        return {};
    }

    OrderBook& book = getOrCreateBook(order.symbol);
    std::shared_mutex& lock = getOrCreateLock(order.symbol);

    emit(OrderEvent::received(order));
    auto startTime = std::chrono::steady_clock::now();

    // 3. Acquire per-symbol EXCLUSIVE lock and match
    std::vector<Trade> trades;
    {
        std::unique_lock<std::shared_mutex> writeLock(lock);
        trades = book.match(order);
    }

    auto endTime = std::chrono::steady_clock::now();
    long latencyMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime).count();

    spdlog::info("Matched order {} in {}µs, {} trades generated",
                 order.orderId, latencyMicros, trades.size());

    // 4. Emit fill events
    for (const auto& trade : trades) {
        FillEvent fill{trade, latencyMicros};
        fillConsumer_(fill);
        emit(OrderEvent::filled(order, trade));
    }

    // 5. Check stop-limit triggers after each trade
    if (!trades.empty()) {
        std::unique_lock<std::shared_mutex> writeLock(lock);
        checkStopTriggers(order.symbol, book);
    }

    // 6. Emit resting confirmation if limit order didn't match
    if (order.type == OrderType::LIMIT && trades.empty()) {
        emit(OrderEvent::resting(order));
    }

    return trades;
}

// ---------------------------------------------------------------
//  ORDER CANCELLATION
// ---------------------------------------------------------------

bool MatchingEngine::cancel(const std::string& symbol, const std::string& orderId) {
    auto bookIt = books_.find(symbol);
    if (bookIt == books_.end()) return false;

    auto lockIt = locks_.find(symbol);
    if (lockIt == locks_.end()) return false;

    std::unique_lock<std::shared_mutex> writeLock(*lockIt->second);
    bool removed = bookIt->second->removeFromBook(orderId);
    if (removed) {
        spdlog::info("Cancelled order {} on {}", orderId, symbol);
    }
    return removed;
}

// ---------------------------------------------------------------
//  READ QUERIES (uses SHARED lock — concurrent reads OK)
// ---------------------------------------------------------------

std::optional<OrderBookSnapshot> MatchingEngine::getSnapshot(
        const std::string& symbol, int depth) {
    auto bookIt = books_.find(symbol);
    if (bookIt == books_.end()) return std::nullopt;

    auto lockIt = locks_.find(symbol);
    if (lockIt == locks_.end()) return std::nullopt;

    std::shared_lock<std::shared_mutex> readLock(*lockIt->second);
    return bookIt->second->snapshot(depth);
}

// ---------------------------------------------------------------
//  STOP-LIMIT TRIGGER PROCESSING
// ---------------------------------------------------------------

void MatchingEngine::checkStopTriggers(const std::string& symbol, OrderBook& book) {
    auto it = stopOrders_.find(symbol);
    if (it == stopOrders_.end()) return;

    auto lastPrice = book.lastTradePriceValue();
    if (!lastPrice) return;

    auto& pending = it->second;
    std::vector<Order> toActivate;

    pending.erase(
        std::remove_if(pending.begin(), pending.end(),
            [&](const Order& stop) {
                bool triggered = stop.isBuy()
                    ? *lastPrice >= stop.stopPrice
                    : *lastPrice <= stop.stopPrice;
                if (triggered) toActivate.push_back(stop);
                return triggered;
            }),
        pending.end());

    // Re-submit triggered stops as LIMIT orders (recursive — acquires lock again)
    for (const auto& stop : toActivate) {
        Order limitOrder = Order::create(
            stop.accountId, stop.symbol, stop.side,
            OrderType::LIMIT, stop.price, 0, stop.remainingQuantity);
        spdlog::info("Stop order {} triggered at {}, submitting as LIMIT",
                     stop.orderId, price_to_string(*lastPrice));
        // Note: this is called while holding the write lock, so we match directly
        book.match(limitOrder);
    }
}

// ---------------------------------------------------------------
//  CONTROL
// ---------------------------------------------------------------

void MatchingEngine::halt() {
    halted_.store(true, std::memory_order_relaxed);
    spdlog::warn("ENGINE HALTED — global kill switch activated");
}

void MatchingEngine::resume() {
    halted_.store(false, std::memory_order_relaxed);
    spdlog::info("Engine resumed");
}

void MatchingEngine::setEventConsumer(std::function<void(const OrderEvent&)> consumer) {
    eventConsumer_ = std::move(consumer);
}

void MatchingEngine::setFillConsumer(std::function<void(const FillEvent&)> consumer) {
    fillConsumer_ = std::move(consumer);
}

void MatchingEngine::emit(const OrderEvent& event) {
    try {
        eventConsumer_(event);
    } catch (const std::exception& e) {
        spdlog::error("Event consumer error: {}", e.what());
    }
}

} // namespace titanx
