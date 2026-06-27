#pragma once

#include <string>
#include <unordered_map>

// Forward declare hiredis types
struct redisContext;

namespace titanx {

/**
 * ============================================================
 * TitanX RedisPositionCache — Hot-Path State (C++17)
 * ============================================================
 *
 * Redis serves as the hot-path store for:
 *   1. Account positions: real-time net position per account per symbol
 *   2. Order status cache: O(1) order status lookup
 *   3. Pub/sub fill broadcasts: real-time fill notifications
 *
 * Why Redis over PostgreSQL?
 *   PostgreSQL P99 ~5ms. Redis P99 ~0.1ms.
 *   At 100K orders/min, we cannot afford 5ms per position check.
 *
 * Key schema:
 *   pos:{accountId}:{symbol}   → net position (signed integer)
 *   order:{orderId}:status     → ORDER_STATUS string
 *   pubsub channel: fills:{symbol}
 */
class RedisPositionCache {
public:
    explicit RedisPositionCache(const std::string& host, int port = 6379);
    ~RedisPositionCache();

    // ---- Position tracking ----
    void updatePosition(const std::string& accountId,
                        const std::string& symbol, int64_t delta);
    int64_t getPosition(const std::string& accountId,
                        const std::string& symbol);

    // ---- Order status cache ----
    void setOrderStatus(const std::string& orderId, const std::string& status);
    std::string getOrderStatus(const std::string& orderId);

    // ---- Pub/sub fill broadcast ----
    void publishFill(const std::string& symbol, const std::string& tradeJson);

    void close();

private:
    redisContext* ctx_;

    std::string positionKey(const std::string& accountId,
                            const std::string& symbol) const;
    std::string orderKey(const std::string& orderId) const;
};

} // namespace titanx
