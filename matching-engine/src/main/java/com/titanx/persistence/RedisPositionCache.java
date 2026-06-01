package com.titanx.persistence;

import io.lettuce.core.RedisClient;
import io.lettuce.core.api.StatefulRedisConnection;
import io.lettuce.core.api.sync.RedisCommands;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.Map;

/**
 * ============================================================
 * TitanX RedisPositionCache — Phase 3: Hot-Path State
 * ============================================================
 *
 * Redis serves as the hot-path store for two things:
 *
 * 1. Open order state:   "is orderId X still active?" → O(1) lookup
 * 2. Account positions:  real-time net position per account per symbol
 *
 * Why Redis and not the database?
 *  "PostgreSQL P99 is ~5ms for a write. Redis P99 is ~0.1ms.
 *   On the matching hot path (100K orders/sec), we cannot afford 5ms
 *   per position check. Redis gives us the speed with acceptable
 *   durability (AOF persistence, replica failover)."
 *
 * Redis key schema:
 *   pos:{accountId}:{symbol}   → net position (integer, signed)
 *   order:{orderId}:status     → ORDER_STATUS enum string
 *   pubsub channel: fills:{symbol}
 */
public class RedisPositionCache {

    private static final Logger log = LoggerFactory.getLogger(RedisPositionCache.class);

    private final RedisClient client;
    private final StatefulRedisConnection<String, String> connection;
    private final RedisCommands<String, String> commands;

    public RedisPositionCache(String redisUri) {
        this.client = RedisClient.create(redisUri);
        this.connection = client.connect();
        this.commands = connection.sync();
        log.info("RedisPositionCache connected to {}", redisUri);
    }

    // ---- Position tracking ----

    public void updatePosition(String accountId, String symbol, long delta) {
        String key = positionKey(accountId, symbol);
        commands.incrby(key, delta);
    }

    public long getPosition(String accountId, String symbol) {
        String val = commands.get(positionKey(accountId, symbol));
        return val == null ? 0L : Long.parseLong(val);
    }

    // ---- Order status cache ----

    public void setOrderStatus(String orderId, String status) {
        commands.setex(orderKey(orderId), 3600, status); // 1hr TTL
    }

    public String getOrderStatus(String orderId) {
        return commands.get(orderKey(orderId));
    }

    // ---- Pub/Sub fill broadcast ----

    /**
     * Publish a fill notification to all WebSocket subscribers.
     * Channel: fills:{symbol}
     * Message: JSON payload of the trade
     */
    public void publishFill(String symbol, String tradeJson) {
        long subscribers = commands.publish("fills:" + symbol, tradeJson);
        log.debug("Published fill to fills:{}, {} subscribers", symbol, subscribers);
    }

    // ---- Flush / Admin ----

    public Map<String, String> getPositionSummary(String accountId) {
        // In production: use HSCAN over a hash key per account
        // Simplified: assume symbols are known
        return Map.of();
    }

    public void close() {
        connection.close();
        client.shutdown();
    }

    private String positionKey(String accountId, String symbol) {
        return "pos:" + accountId + ":" + symbol;
    }

    private String orderKey(String orderId) {
        return "order:" + orderId + ":status";
    }
}
