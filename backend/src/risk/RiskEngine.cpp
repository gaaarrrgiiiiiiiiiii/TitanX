#include "titanx/risk/RiskEngine.hpp"
#include <cmath>

namespace titanx {

RiskEngine::RiskEngine(CircuitBreaker& circuitBreaker)
    : circuitBreaker_(circuitBreaker) {}

// ---------------------------------------------------------------
//  MAIN ENTRY POINT
// ---------------------------------------------------------------

RiskResult RiskEngine::checkPreTrade(const Order& order) {
    totalChecked_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(mutex_);

    // 1. Circuit breaker check (per-symbol halt)
    if (circuitBreaker_.isHalted(order.symbol)) {
        return reject("CIRCUIT_BREAKER_HALT");
    }

    // 2. Duplicate order ID
    if (!seenOrderIds_.insert(order.orderId).second) {
        return reject("DUPLICATE_ORDER_ID");
    }

    // 3. Notional value check (price × quantity)
    if (order.price > 0) {
        // Fixed-point: notional = (price * quantity) — already in fixed-point scale
        int64_t notional = order.price * order.quantity;
        if (notional > MAX_NOTIONAL_FIXED) {
            return reject("NOTIONAL_LIMIT_EXCEEDED");
        }
    }

    // 4. Position limit check
    int64_t netPos = 0;
    auto acctIt = positions_.find(order.accountId);
    if (acctIt != positions_.end()) {
        auto symIt = acctIt->second.find(order.symbol);
        if (symIt != acctIt->second.end()) {
            netPos = symIt->second;
        }
    }

    int64_t projectedPosition = order.isBuy()
        ? netPos + order.quantity
        : netPos - order.quantity;

    if (std::abs(projectedPosition) > MAX_POSITION_SHARES) {
        return reject("POSITION_LIMIT_EXCEEDED");
    }

    // 5. Fat-finger protection (only for limit orders with a price)
    if (order.price > 0) {
        auto lastIt = lastTradePrices_.find(order.symbol);
        if (lastIt != lastTradePrices_.end() && lastIt->second > 0) {
            int64_t lastPrice = lastIt->second;
            int64_t absDiff = std::abs(order.price - lastPrice);
            int64_t deviationBps = (absDiff * 10000) / lastPrice;
            if (deviationBps > FAT_FINGER_BPS) {
                return reject("FAT_FINGER_PRICE_DEVIATION");
            }
        }
    }

    // 6. Self-trade prevention — check if opposite side has order from same account
    if (order.isBuy()) {
        auto it = activeSellOrders_.find(order.accountId);
        if (it != activeSellOrders_.end() && !it->second.empty()) {
            return reject("SELF_TRADE_PREVENTION");
        }
    } else {
        auto it = activeBuyOrders_.find(order.accountId);
        if (it != activeBuyOrders_.end() && !it->second.empty()) {
            return reject("SELF_TRADE_PREVENTION");
        }
    }

    // All checks passed
    trackOrder(order);
    return RiskResult::approve();
}

// ---------------------------------------------------------------
//  POST-TRADE UPDATES
// ---------------------------------------------------------------

void RiskEngine::recordFill(const std::string& accountId,
                            const std::string& symbol,
                            OrderSide side, int64_t quantity) {
    std::lock_guard<std::mutex> guard(mutex_);
    int64_t delta = (side == OrderSide::BUY) ? quantity : -quantity;
    positions_[accountId][symbol] += delta;
}

void RiskEngine::updateLastTradePrice(const std::string& symbol, int64_t price) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        lastTradePrices_[symbol] = price;
    }
    circuitBreaker_.recordTrade(symbol, price);
}

// ---------------------------------------------------------------
//  QUERIES
// ---------------------------------------------------------------

int64_t RiskEngine::getNetPosition(const std::string& accountId,
                                   const std::string& symbol) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto acctIt = positions_.find(accountId);
    if (acctIt == positions_.end()) return 0;
    auto symIt = acctIt->second.find(symbol);
    if (symIt == acctIt->second.end()) return 0;
    return symIt->second;
}

// ---------------------------------------------------------------
//  INTERNAL
// ---------------------------------------------------------------

void RiskEngine::trackOrder(const Order& order) {
    if (order.isBuy()) {
        activeBuyOrders_[order.accountId].insert(order.orderId);
    } else {
        activeSellOrders_[order.accountId].insert(order.orderId);
    }
}

RiskResult RiskEngine::reject(const std::string& reason) {
    totalRejected_.fetch_add(1, std::memory_order_relaxed);
    return RiskResult::rejected(reason);
}

} // namespace titanx
