#include "titanx/engine/OrderBook.hpp"
#include <algorithm>

namespace titanx {

OrderBook::OrderBook(const std::string& symbol)
    : symbol_(symbol) {}

// ---------------------------------------------------------------
//  MAIN MATCHING ENTRY POINT
// ---------------------------------------------------------------

std::vector<Trade> OrderBook::match(const Order& incoming) {
    switch (incoming.type) {
        case OrderType::MARKET:     return matchMarket(incoming);
        case OrderType::LIMIT:      return matchLimit(incoming);
        case OrderType::IOC:        return matchIoc(incoming);
        case OrderType::FOK:        return matchFok(incoming);
        case OrderType::STOP_LIMIT:
            // Stop-limit orders are held in a separate trigger map
            // and injected as LIMIT orders when stopPrice is touched.
            return {};
    }
    return {};
}

// ---------------------------------------------------------------
//  MARKET ORDER: consume opposite side greedily
// ---------------------------------------------------------------

std::vector<Trade> OrderBook::matchMarket(const Order& aggressor) {
    std::vector<Trade> trades;

    // If buying, match against asks (sellers). If selling, match against bids.
    auto matchSide = [&](auto& oppositeSide) {
        int64_t remainingQty = aggressor.remainingQuantity;

        while (remainingQty > 0 && !oppositeSide.empty()) {
            auto bestIt = oppositeSide.begin();
            int64_t bestPrice = bestIt->first;
            auto& levelQueue = bestIt->second;

            while (remainingQty > 0 && !levelQueue.empty()) {
                Order& passive = levelQueue.front();
                int64_t fillQty = std::min(remainingQty, passive.remainingQuantity);

                Trade trade = Trade::create(aggressor, passive, bestPrice, fillQty);
                trades.push_back(trade);
                lastTradePrice_ = bestPrice;
                remainingQty -= fillQty;

                // Update passive order (copy-on-modify)
                Order updatedPassive = passive.withRemainingQuantity(
                    passive.remainingQuantity - fillQty);
                levelQueue.pop_front(); // remove old

                if (updatedPassive.remainingQuantity > 0) {
                    levelQueue.push_front(updatedPassive); // put back if partial
                    orderIndex_[updatedPassive.orderId] = updatedPassive;
                } else {
                    orderIndex_.erase(passive.orderId);
                }
            }

            if (levelQueue.empty()) {
                oppositeSide.erase(bestIt);
            }
        }
    };

    if (aggressor.isBuy()) {
        matchSide(asks_);
    } else {
        matchSide(bids_);
    }

    return trades;
}

// ---------------------------------------------------------------
//  LIMIT ORDER: match if crosses spread, else insert into book
// ---------------------------------------------------------------

std::vector<Trade> OrderBook::matchLimit(const Order& aggressor) {
    std::vector<Trade> trades;

    auto matchSide = [&](auto& oppositeSide) {
        int64_t remainingQty = aggressor.remainingQuantity;

        while (remainingQty > 0 && !oppositeSide.empty()) {
            auto bestIt = oppositeSide.begin();
            int64_t bestPrice = bestIt->first;

            // Check if the order crosses the spread
            bool crosses;
            if (aggressor.isBuy()) {
                crosses = aggressor.price >= bestPrice;
            } else {
                crosses = aggressor.price <= bestPrice;
            }

            if (!crosses) break; // no more crossable levels

            auto& levelQueue = bestIt->second;
            while (remainingQty > 0 && !levelQueue.empty()) {
                Order& passive = levelQueue.front();
                int64_t fillQty = std::min(remainingQty, passive.remainingQuantity);

                Trade trade = Trade::create(aggressor, passive, bestPrice, fillQty);
                trades.push_back(trade);
                lastTradePrice_ = bestPrice;
                remainingQty -= fillQty;

                Order updatedPassive = passive.withRemainingQuantity(
                    passive.remainingQuantity - fillQty);
                levelQueue.pop_front();

                if (updatedPassive.remainingQuantity > 0) {
                    levelQueue.push_front(updatedPassive);
                    orderIndex_[updatedPassive.orderId] = updatedPassive;
                } else {
                    orderIndex_.erase(passive.orderId);
                }
            }

            if (levelQueue.empty()) {
                oppositeSide.erase(bestIt);
            }
        }

        // If not fully filled, rest the unfilled portion in the book
        if (remainingQty > 0) {
            Order restingOrder = aggressor.withRemainingQuantity(remainingQty);
            insertIntoBook(restingOrder);
        }
    };

    if (aggressor.isBuy()) {
        matchSide(asks_);
    } else {
        matchSide(bids_);
    }

    return trades;
}

// ---------------------------------------------------------------
//  IOC: match what's available, cancel the rest
// ---------------------------------------------------------------

std::vector<Trade> OrderBook::matchIoc(const Order& aggressor) {
    std::vector<Trade> trades = matchLimit(aggressor);
    // Remove the resting portion we just inserted (if any)
    removeFromBook(aggressor.orderId);
    return trades;
}

// ---------------------------------------------------------------
//  FOK: check full quantity available before executing
// ---------------------------------------------------------------

std::vector<Trade> OrderBook::matchFok(const Order& aggressor) {
    int64_t available = availableQuantityAtOrBetter(aggressor);
    if (available < aggressor.quantity) {
        return {}; // Cannot fill completely — cancel
    }
    return matchMarket(aggressor); // FOK matched as market once confirmed
}

// ---------------------------------------------------------------
//  BOOK OPERATIONS
// ---------------------------------------------------------------

void OrderBook::insertIntoBook(const Order& order) {
    if (order.isBuy()) {
        bids_[order.price].push_back(order);
    } else {
        asks_[order.price].push_back(order);
    }
    orderIndex_[order.orderId] = order;
}

bool OrderBook::removeFromBook(const std::string& orderId) {
    auto it = orderIndex_.find(orderId);
    if (it == orderIndex_.end()) return false;

    Order order = it->second;
    orderIndex_.erase(it);

    if (order.price == 0) return false; // market orders have no price

    if (order.isBuy()) {
        auto levelIt = bids_.find(order.price);
        if (levelIt == bids_.end()) return false;
        auto& queue = levelIt->second;
        queue.erase(
            std::remove_if(queue.begin(), queue.end(),
                [&](const Order& o) { return o.orderId == orderId; }),
            queue.end());
        if (queue.empty()) bids_.erase(levelIt);
    } else {
        auto levelIt = asks_.find(order.price);
        if (levelIt == asks_.end()) return false;
        auto& queue = levelIt->second;
        queue.erase(
            std::remove_if(queue.begin(), queue.end(),
                [&](const Order& o) { return o.orderId == orderId; }),
            queue.end());
        if (queue.empty()) asks_.erase(levelIt);
    }

    return true;
}

// ---------------------------------------------------------------
//  QUERIES
// ---------------------------------------------------------------

int64_t OrderBook::availableQuantityAtOrBetter(const Order& aggressor) const {
    int64_t total = 0;

    auto countSide = [&](const auto& oppositeSide) {
        for (const auto& [levelPrice, queue] : oppositeSide) {
            bool crosses;
            if (aggressor.isBuy()) {
                crosses = aggressor.price == 0 || aggressor.price >= levelPrice;
            } else {
                crosses = aggressor.price == 0 || aggressor.price <= levelPrice;
            }
            if (!crosses) break;
            for (const auto& o : queue) {
                total += o.remainingQuantity;
            }
        }
    };

    if (aggressor.isBuy()) {
        countSide(asks_);
    } else {
        countSide(bids_);
    }

    return total;
}

std::optional<int64_t> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<int64_t> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<int64_t> OrderBook::spread() const {
    auto bid = bestBid();
    auto ask = bestAsk();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}

std::optional<int64_t> OrderBook::lastTradePriceValue() const {
    return lastTradePrice_;
}

OrderBookSnapshot OrderBook::snapshot(int depth) const {
    OrderBookSnapshot snap;
    snap.symbol = symbol_;
    snap.lastTradePrice = lastTradePrice_;

    int count = 0;
    for (const auto& [price, queue] : bids_) {
        if (count++ >= depth) break;
        int64_t qty = 0;
        for (const auto& o : queue) qty += o.remainingQuantity;
        snap.bids.push_back({price, qty, static_cast<int>(queue.size())});
    }

    count = 0;
    for (const auto& [price, queue] : asks_) {
        if (count++ >= depth) break;
        int64_t qty = 0;
        for (const auto& o : queue) qty += o.remainingQuantity;
        snap.asks.push_back({price, qty, static_cast<int>(queue.size())});
    }

    return snap;
}

} // namespace titanx
