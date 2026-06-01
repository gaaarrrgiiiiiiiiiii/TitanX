package com.titanx.risk;

import com.titanx.model.Order;
import com.titanx.model.OrderSide;

import java.math.BigDecimal;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/**
 * ============================================================
 * TitanX RiskEngine — Phase 2: Pre-Trade Risk Controls
 * ============================================================
 *
 * Pre-trade checks run BEFORE an order enters the matching engine.
 * This is the correct architecture: reject bad orders at the gateway,
 * not inside the hot matching loop.
 *
 * Checks implemented:
 *  1. Position limits (max net long/short per account)
 *  2. Notional value limits (max order size in USD)
 *  3. Self-trade prevention (same account cannot be both sides)
 *  4. Duplicate order ID detection (idempotency)
 *  5. Fat-finger protection (price deviation from last trade)
 *  6. Global kill switch integration
 *
 * Interview point:
 *  "Self-trade prevention is required by RegNMS. The engine tracks pending
 *   bids and asks per account per symbol. If an incoming order would match
 *   against the same account's resting order, we cancel-and-replace the
 *   passive side or reject the aggressor — exchange-specific policy choice."
 */
public class RiskEngine {

    // ---- Configuration (would be in a config service in production) ----
    private static final long MAX_POSITION_SHARES     = 10_000L;
    private static final BigDecimal MAX_NOTIONAL_USD  = new BigDecimal("5_000_000");
    private static final BigDecimal FAT_FINGER_FACTOR = new BigDecimal("0.10"); // 10% deviation

    // ---- State ----
    // accountId → symbol → net position (positive=long, negative=short)
    private final Map<String, Map<String, Long>> positions = new ConcurrentHashMap<>();

    // accountId → set of active order IDs on buy side / sell side
    private final Map<String, Set<String>> activeBuyOrders  = new ConcurrentHashMap<>();
    private final Map<String, Set<String>> activeSellOrders = new ConcurrentHashMap<>();

    // Seen order IDs for idempotency
    private final Set<String> seenOrderIds = ConcurrentHashMap.newKeySet();

    // Last trade prices per symbol (updated by MatchingEngine after fills)
    private final Map<String, BigDecimal> lastTradePrices = new ConcurrentHashMap<>();

    // Counters for observability
    private final AtomicLong totalChecked  = new AtomicLong();
    private final AtomicLong totalRejected = new AtomicLong();

    // CircuitBreaker integration
    private final CircuitBreaker circuitBreaker;

    public RiskEngine(CircuitBreaker circuitBreaker) {
        this.circuitBreaker = circuitBreaker;
    }

    // ---------------------------------------------------------------
    //  MAIN ENTRY POINT
    // ---------------------------------------------------------------

    public RiskResult checkPreTrade(Order order) {
        totalChecked.incrementAndGet();

        // 1. Circuit breaker check (per-symbol halt)
        if (circuitBreaker.isHalted(order.symbol())) {
            return reject("CIRCUIT_BREAKER_HALT");
        }

        // 2. Duplicate order ID
        if (!seenOrderIds.add(order.orderId())) {
            return reject("DUPLICATE_ORDER_ID");
        }

        // 3. Notional value check
        if (order.price() != null) {
            BigDecimal notional = order.price().multiply(BigDecimal.valueOf(order.quantity()));
            if (notional.compareTo(MAX_NOTIONAL_USD) > 0) {
                return reject("NOTIONAL_LIMIT_EXCEEDED");
            }
        }

        // 4. Position limit check
        long netPosition = getNetPosition(order.accountId(), order.symbol());
        long projectedPosition = order.isBuy()
                ? netPosition + order.quantity()
                : netPosition - order.quantity();
        if (Math.abs(projectedPosition) > MAX_POSITION_SHARES) {
            return reject("POSITION_LIMIT_EXCEEDED");
        }

        // 5. Fat-finger protection (only for limit orders with a price)
        if (order.price() != null) {
            BigDecimal lastPrice = lastTradePrices.get(order.symbol());
            if (lastPrice != null) {
                BigDecimal deviation = order.price().subtract(lastPrice)
                        .abs().divide(lastPrice, 4, java.math.RoundingMode.HALF_UP);
                if (deviation.compareTo(FAT_FINGER_FACTOR) > 0) {
                    return reject("FAT_FINGER_PRICE_DEVIATION");
                }
            }
        }

        // 6. Self-trade prevention — check if opposite side has order from same account
        // (Simplified: in production this requires O(1) lookup into the book's account index)
        // We track pending orders per account and flag potential self-trades
        Set<String> oppositeOrders = order.isBuy()
                ? activeSellOrders.getOrDefault(order.accountId(), Set.of())
                : activeBuyOrders.getOrDefault(order.accountId(), Set.of());
        if (!oppositeOrders.isEmpty()) {
            // Policy: reject aggressor (exchange can also cancel passive side)
            return reject("SELF_TRADE_PREVENTION");
        }

        // All checks passed
        trackOrder(order);
        return RiskResult.approved();
    }

    // ---------------------------------------------------------------
    //  POST-TRADE UPDATES (called by MatchingEngine after fills)
    // ---------------------------------------------------------------

    public void recordFill(String accountId, String symbol, OrderSide side, long quantity) {
        Map<String, Long> accountPositions = positions.computeIfAbsent(
                accountId, k -> new ConcurrentHashMap<>());
        accountPositions.merge(symbol, side == OrderSide.BUY ? quantity : -quantity, Long::sum);
    }

    public void updateLastTradePrice(String symbol, BigDecimal price) {
        lastTradePrices.put(symbol, price);
        circuitBreaker.recordTrade(symbol, price);
    }

    // ---------------------------------------------------------------
    //  QUERIES
    // ---------------------------------------------------------------

    public long getNetPosition(String accountId, String symbol) {
        return positions
                .getOrDefault(accountId, Map.of())
                .getOrDefault(symbol, 0L);
    }

    public long totalChecked()  { return totalChecked.get(); }
    public long totalRejected() { return totalRejected.get(); }

    // ---------------------------------------------------------------
    //  INTERNAL
    // ---------------------------------------------------------------

    private void trackOrder(Order order) {
        Set<String> activeOrders = order.isBuy()
                ? activeBuyOrders.computeIfAbsent(order.accountId(), k -> ConcurrentHashMap.newKeySet())
                : activeSellOrders.computeIfAbsent(order.accountId(), k -> ConcurrentHashMap.newKeySet());
        activeOrders.add(order.orderId());
    }

    private RiskResult reject(String reason) {
        totalRejected.incrementAndGet();
        return RiskResult.rejected(reason);
    }
}
