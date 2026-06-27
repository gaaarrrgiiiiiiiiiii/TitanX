#pragma once

#include "titanx/model/Order.hpp"
#include "titanx/model/Trade.hpp"

#include <nlohmann/json.hpp>
#include <map>
#include <deque>
#include <unordered_map>
#include <vector>
#include <optional>
#include <string>
#include <functional>

namespace titanx {

/**
 * ============================================================
 * TitanX OrderBook — Core Data Structure (C++17)
 * ============================================================
 *
 * Design:
 *   BIDS: std::map<int64_t, std::deque<Order>, std::greater<>>
 *         → DESCENDING key order, best bid = highest price = first entry
 *   ASKS: std::map<int64_t, std::deque<Order>>
 *         → ASCENDING key order, best ask = lowest price = first entry
 *
 * Within each price level, orders are in a deque (FIFO).
 * This implements price-time priority (NYSE, NASDAQ, CME standard).
 *
 * std::deque over std::priority_queue because:
 *   - We need FIFO (arrival order), not arbitrary priority
 *   - Deque is contiguous-ish memory, cache-friendly
 *   - O(1) push_back, O(1) pop_front
 *
 * Thread safety:
 *   NOT thread-safe. External locking (per-symbol shared_mutex in
 *   MatchingEngine) is the caller's responsibility.
 */

struct PriceLevel {
    int64_t price;
    int64_t totalQuantity;
    int     orderCount;

    nlohmann::json to_json() const {
        return {
            {"price", price_to_string(price)},
            {"totalQuantity", totalQuantity},
            {"orderCount", orderCount}
        };
    }
};

struct OrderBookSnapshot {
    std::string            symbol;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    std::optional<int64_t>  lastTradePrice;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["symbol"] = symbol;
        j["bids"] = nlohmann::json::array();
        for (auto& lvl : bids) j["bids"].push_back(lvl.to_json());
        j["asks"] = nlohmann::json::array();
        for (auto& lvl : asks) j["asks"].push_back(lvl.to_json());
        j["lastTradePrice"] = lastTradePrice
            ? price_to_string(*lastTradePrice) : nullptr;
        return j;
    }
};

class OrderBook {
public:
    explicit OrderBook(const std::string& symbol);

    // ---- Main matching entry point ----
    std::vector<Trade> match(const Order& incoming);

    // ---- Book operations ----
    void insertIntoBook(const Order& order);
    bool removeFromBook(const std::string& orderId);

    // ---- Queries ----
    std::optional<int64_t> bestBid() const;
    std::optional<int64_t> bestAsk() const;
    std::optional<int64_t> spread() const;
    std::optional<int64_t> lastTradePriceValue() const;

    int bidLevels()  const { return static_cast<int>(bids_.size()); }
    int askLevels()  const { return static_cast<int>(asks_.size()); }
    int totalOrders() const { return static_cast<int>(orderIndex_.size()); }
    const std::string& symbol() const { return symbol_; }

    // ---- Snapshot for WebSocket market data feed ----
    OrderBookSnapshot snapshot(int depth) const;

private:
    std::string symbol_;

    // Best bid = highest price → use std::greater for descending order
    std::map<int64_t, std::deque<Order>, std::greater<int64_t>> bids_;

    // Best ask = lowest price → natural ascending order
    std::map<int64_t, std::deque<Order>> asks_;

    // O(1) lookup by orderId for cancellation
    std::unordered_map<std::string, Order> orderIndex_;

    // Last trade price for circuit breaker / stop-limit triggers
    std::optional<int64_t> lastTradePrice_;

    // ---- Matching algorithms ----
    std::vector<Trade> matchMarket(const Order& aggressor);
    std::vector<Trade> matchLimit(const Order& aggressor);
    std::vector<Trade> matchIoc(const Order& aggressor);
    std::vector<Trade> matchFok(const Order& aggressor);

    // ---- Helpers ----
    int64_t availableQuantityAtOrBetter(const Order& aggressor) const;
};

} // namespace titanx
