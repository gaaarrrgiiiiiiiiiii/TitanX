package com.titanx.model;

import java.math.BigDecimal;
import java.time.Instant;
import java.util.UUID;

/**
 * Immutable order record.
 * Uses Java 21 records for clean, concise domain modeling.
 *
 * Interview point: Why BigDecimal for price?
 * → Floating-point representation errors (0.1 + 0.2 ≠ 0.3) are unacceptable
 *   in financial systems. BigDecimal gives exact decimal arithmetic.
 */
public record Order(
        String orderId,
        String accountId,
        String symbol,
        OrderSide side,
        OrderType type,
        BigDecimal price,       // null for MARKET orders
        BigDecimal stopPrice,   // non-null for STOP_LIMIT orders
        long quantity,
        long remainingQuantity,
        OrderStatus status,
        Instant createdAt,
        Instant updatedAt
) implements Comparable<Order> {

    /**
     * Factory method for new orders.
     * remainingQuantity starts equal to quantity.
     */
    public static Order create(String accountId, String symbol, OrderSide side,
                               OrderType type, BigDecimal price, BigDecimal stopPrice,
                               long quantity) {
        String id = UUID.randomUUID().toString();
        Instant now = Instant.now();
        return new Order(id, accountId, symbol, side, type, price, stopPrice,
                quantity, quantity, OrderStatus.PENDING, now, now);
    }

    /**
     * Returns a new Order with updated remainingQuantity.
     * Records are immutable — we return a copy (functional update).
     */
    public Order withRemainingQuantity(long newRemaining) {
        OrderStatus newStatus = newRemaining == 0 ? OrderStatus.FILLED
                : newRemaining < quantity ? OrderStatus.PARTIALLY_FILLED
                : this.status;
        return new Order(orderId, accountId, symbol, side, type, price, stopPrice,
                quantity, newRemaining, newStatus, createdAt, Instant.now());
    }

    public Order withStatus(OrderStatus newStatus) {
        return new Order(orderId, accountId, symbol, side, type, price, stopPrice,
                quantity, remainingQuantity, newStatus, createdAt, Instant.now());
    }

    public boolean isBuy()  { return side == OrderSide.BUY; }
    public boolean isSell() { return side == OrderSide.SELL; }
    public boolean isMarket() { return type == OrderType.MARKET; }
    public boolean isActive()  {
        return status == OrderStatus.PENDING || status == OrderStatus.PARTIALLY_FILLED;
    }

    /**
     * FIFO within a price level: earlier orders have priority.
     * This implements price-time priority — the standard exchange priority rule.
     */
    @Override
    public int compareTo(Order other) {
        return this.createdAt.compareTo(other.createdAt);
    }

    @Override
    public String toString() {
        return String.format("Order{id=%s, acct=%s, %s %s %s qty=%d/%d @ %s}",
                orderId.substring(0, 8), accountId, side, type, symbol,
                remainingQuantity, quantity, price != null ? price : "MKT");
    }
}
