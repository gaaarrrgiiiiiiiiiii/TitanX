package com.titanx.event;

import com.titanx.model.Order;
import com.titanx.model.Trade;

import java.time.Instant;

/**
 * ============================================================
 * TitanX OrderEvent — Phase 3: Event Sourcing
 * ============================================================
 *
 * Every state change in the system produces an immutable OrderEvent.
 * Events are appended to the `order_events` table in PostgreSQL.
 *
 * Why event sourcing?
 *  "Instead of updating a row in an `orders` table, we append events.
 *   Current state is a projection over events. This gives us:
 *   1. Crash recovery — replay events to reconstruct book state
 *   2. Full audit trail — required by MiFID II and RegNMS
 *   3. Time-travel queries — 'what was the order book at 14:32:07?'
 *   Bloomberg Terminal and ICE both use this pattern."
 *
 * Event types mirror the FIX protocol execution report reasons.
 */
public record OrderEvent(
        String eventId,
        EventType type,
        String orderId,
        String accountId,
        String symbol,
        Order  orderSnapshot,   // full order state at this point
        Trade  trade,           // non-null for FILLED events
        String rejectionReason, // non-null for REJECTED events
        long   latencyMicros,
        Instant occurredAt
) {
    public static OrderEvent received(Order order) {
        return build(EventType.ORDER_RECEIVED, order, null, null, 0);
    }

    public static OrderEvent resting(Order order) {
        return build(EventType.ORDER_RESTING, order, null, null, 0);
    }

    public static OrderEvent filled(Order order, Trade trade) {
        return build(EventType.ORDER_FILLED, order, trade, null, 0);
    }

    public static OrderEvent partiallyFilled(Order order, Trade trade) {
        return build(EventType.ORDER_PARTIALLY_FILLED, order, trade, null, 0);
    }

    public static OrderEvent cancelled(Order order) {
        return build(EventType.ORDER_CANCELLED, order, null, null, 0);
    }

    public static OrderEvent rejected(Order order, String reason) {
        return build(EventType.ORDER_REJECTED, order, null, reason, 0);
    }

    private static OrderEvent build(EventType type, Order order, Trade trade,
                                    String reason, long latencyMicros) {
        return new OrderEvent(
                java.util.UUID.randomUUID().toString(),
                type,
                order.orderId(),
                order.accountId(),
                order.symbol(),
                order,
                trade,
                reason,
                latencyMicros,
                Instant.now()
        );
    }
}
