package com.titanx.event;

import com.titanx.model.Trade;

import java.time.Instant;

/**
 * Represents a fill notification published to Redis pub/sub.
 * Consumed by WebSocket handlers to push real-time updates to clients.
 */
public record FillEvent(
        Trade trade,
        long matchingLatencyMicros,
        Instant publishedAt
) {
    public FillEvent(Trade trade, long latencyMicros) {
        this(trade, latencyMicros, Instant.now());
    }
}
