package com.titanx.model;

import java.math.BigDecimal;
import java.time.Instant;
import java.util.UUID;

/**
 * Represents a completed trade between two orders.
 * A single aggressive order can generate multiple Trade records
 * if it crosses multiple price levels.
 */
public record Trade(
        String tradeId,
        String symbol,
        String aggressorOrderId,   // the order that came in and crossed the spread
        String aggressorAccountId,
        String passiveOrderId,     // the resting order in the book
        String passiveAccountId,
        BigDecimal price,          // fill price = passive order's limit price
        long quantity,             // shares exchanged in this trade
        OrderSide aggressorSide,
        Instant executedAt
) {
    public static Trade of(Order aggressor, Order passive, BigDecimal fillPrice, long fillQty) {
        return new Trade(
                UUID.randomUUID().toString(),
                aggressor.symbol(),
                aggressor.orderId(),
                aggressor.accountId(),
                passive.orderId(),
                passive.accountId(),
                fillPrice,
                fillQty,
                aggressor.side(),
                Instant.now()
        );
    }

    /** Notional value of this trade in USD */
    public BigDecimal notional() {
        return price.multiply(BigDecimal.valueOf(quantity));
    }

    @Override
    public String toString() {
        return String.format("Trade{%s %s qty=%d @ %s, aggressor=%s, passive=%s}",
                symbol, aggressorSide, quantity, price,
                aggressorOrderId.substring(0, 8), passiveOrderId.substring(0, 8));
    }
}
