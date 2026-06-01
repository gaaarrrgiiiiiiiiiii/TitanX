package com.titanx.engine;

import com.titanx.model.*;

import java.math.BigDecimal;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * ============================================================
 * TitanX OrderBook — Phase 1 Core Data Structure
 * ============================================================
 *
 * Design:
 *   - BIDS:  TreeMap<price, PriorityQueue<Order>> — DESCENDING key order
 *             → best bid is the HIGHEST price (first entry)
 *   - ASKS:  TreeMap<price, PriorityQueue<Order>> — ASCENDING key order
 *             → best ask is the LOWEST price (first entry)
 *
 * Within each price level, orders are in a PriorityQueue sorted by
 * arrival time (price-time priority — standard exchange semantics).
 *
 * Interview talking point:
 *   "I chose TreeMap over HashMap because matching requires sorted
 *    iteration — you must walk price levels in order. TreeMap gives
 *    O(log n) insert/delete and O(1) first/last, which is the right
 *    trade-off for a matching engine."
 *
 * Thread safety:
 *   This class is NOT thread-safe. External locking (per-symbol
 *   ReentrantReadWriteLock in MatchingEngine) is the caller's
 *   responsibility. This keeps the hot path clean.
 */
public class OrderBook {

    private final String symbol;

    // Best bid = highest price → reverse natural order
    private final TreeMap<BigDecimal, PriorityQueue<Order>> bids =
            new TreeMap<>(Comparator.reverseOrder());

    // Best ask = lowest price → natural ascending order
    private final TreeMap<BigDecimal, PriorityQueue<Order>> asks =
            new TreeMap<>();

    // O(1) lookup by orderId for cancellation
    private final Map<String, Order> orderIndex = new ConcurrentHashMap<>();

    private BigDecimal lastTradePrice = null;

    public OrderBook(String symbol) {
        this.symbol = symbol;
    }

    // ---------------------------------------------------------------
    //  MATCHING LOGIC
    // ---------------------------------------------------------------

    /**
     * Match an incoming aggressive order against resting orders.
     * Returns a list of trades generated during matching.
     *
     * Handles: MARKET, LIMIT, IOC, FOK
     */
    public List<Trade> match(Order incoming) {
        return switch (incoming.type()) {
            case MARKET   -> matchMarket(incoming);
            case LIMIT    -> matchLimit(incoming);
            case IOC      -> matchIoc(incoming);
            case FOK      -> matchFok(incoming);
            case STOP_LIMIT -> {
                // Stop-limit orders are held in a separate trigger map
                // and injected as LIMIT orders when stopPrice is touched.
                // For now, insert into book as a pending stop.
                yield List.of();
            }
        };
    }

    /**
     * Market order: consume opposite side greedily until filled or book empty.
     */
    private List<Trade> matchMarket(Order aggressor) {
        List<Trade> trades = new ArrayList<>();
        TreeMap<BigDecimal, PriorityQueue<Order>> oppositeSide =
                aggressor.isBuy() ? asks : bids;

        long remainingQty = aggressor.remainingQuantity();

        while (remainingQty > 0 && !oppositeSide.isEmpty()) {
            Map.Entry<BigDecimal, PriorityQueue<Order>> bestLevel =
                    oppositeSide.firstEntry();
            PriorityQueue<Order> levelQueue = bestLevel.getValue();

            while (remainingQty > 0 && !levelQueue.isEmpty()) {
                Order passive = levelQueue.peek();
                long fillQty = Math.min(remainingQty, passive.remainingQuantity());
                Trade trade = Trade.of(aggressor, passive, bestLevel.getKey(), fillQty);
                trades.add(trade);
                lastTradePrice = bestLevel.getKey();
                remainingQty -= fillQty;

                // Update passive order
                Order updatedPassive = passive.withRemainingQuantity(
                        passive.remainingQuantity() - fillQty);
                levelQueue.poll();  // remove old
                if (updatedPassive.remainingQuantity() > 0) {
                    levelQueue.offer(updatedPassive);  // put back if partial
                    orderIndex.put(updatedPassive.orderId(), updatedPassive);
                } else {
                    orderIndex.remove(passive.orderId());
                }
            }

            if (levelQueue.isEmpty()) {
                oppositeSide.remove(bestLevel.getKey());
            }
        }
        return trades;
    }

    /**
     * Limit order: match if crosses spread, otherwise insert into book.
     *
     * A BUY limit crosses if its price ≥ best ask.
     * A SELL limit crosses if its price ≤ best bid.
     */
    private List<Trade> matchLimit(Order aggressor) {
        List<Trade> trades = new ArrayList<>();
        TreeMap<BigDecimal, PriorityQueue<Order>> oppositeSide =
                aggressor.isBuy() ? asks : bids;

        long remainingQty = aggressor.remainingQuantity();

        while (remainingQty > 0 && !oppositeSide.isEmpty()) {
            Map.Entry<BigDecimal, PriorityQueue<Order>> bestLevel =
                    oppositeSide.firstEntry();
            BigDecimal bestPrice = bestLevel.getKey();

            // Check if the order crosses the spread
            boolean crosses = aggressor.isBuy()
                    ? aggressor.price().compareTo(bestPrice) >= 0
                    : aggressor.price().compareTo(bestPrice) <= 0;

            if (!crosses) break;  // no more crossable levels

            PriorityQueue<Order> levelQueue = bestLevel.getValue();
            while (remainingQty > 0 && !levelQueue.isEmpty()) {
                Order passive = levelQueue.peek();
                long fillQty = Math.min(remainingQty, passive.remainingQuantity());
                Trade trade = Trade.of(aggressor, passive, bestPrice, fillQty);
                trades.add(trade);
                lastTradePrice = bestPrice;
                remainingQty -= fillQty;

                Order updatedPassive = passive.withRemainingQuantity(
                        passive.remainingQuantity() - fillQty);
                levelQueue.poll();
                if (updatedPassive.remainingQuantity() > 0) {
                    levelQueue.offer(updatedPassive);
                    orderIndex.put(updatedPassive.orderId(), updatedPassive);
                } else {
                    orderIndex.remove(passive.orderId());
                }
            }
            if (levelQueue.isEmpty()) {
                oppositeSide.remove(bestPrice);
            }
        }

        // If not fully filled, rest the unfilled portion in the book
        if (remainingQty > 0) {
            Order restingOrder = aggressor.withRemainingQuantity(remainingQty);
            insertIntoBook(restingOrder);
        }

        return trades;
    }

    /**
     * IOC: match what's available, cancel the rest. Never rests in book.
     */
    private List<Trade> matchIoc(Order aggressor) {
        // Temporarily treat as LIMIT for matching, then discard remainder
        List<Trade> trades = matchLimit(aggressor);
        // Remove the resting portion we just inserted (if any)
        removeFromBook(aggressor.orderId());
        return trades;
    }

    /**
     * FOK: check if the full quantity can be filled before executing.
     * If not, reject entirely (zero trades).
     */
    private List<Trade> matchFok(Order aggressor) {
        long available = availableQuantityAtOrBetter(aggressor);
        if (available < aggressor.quantity()) {
            return List.of();  // Cannot fill completely — cancel
        }
        return matchMarket(aggressor);  // FOK matched as market once confirmed
    }

    // ---------------------------------------------------------------
    //  BOOK OPERATIONS
    // ---------------------------------------------------------------

    public void insertIntoBook(Order order) {
        TreeMap<BigDecimal, PriorityQueue<Order>> side =
                order.isBuy() ? bids : asks;
        side.computeIfAbsent(order.price(), p -> new PriorityQueue<>())
            .offer(order);
        orderIndex.put(order.orderId(), order);
    }

    public boolean removeFromBook(String orderId) {
        Order order = orderIndex.remove(orderId);
        if (order == null || order.price() == null) return false;

        TreeMap<BigDecimal, PriorityQueue<Order>> side =
                order.isBuy() ? bids : asks;
        PriorityQueue<Order> level = side.get(order.price());
        if (level == null) return false;

        boolean removed = level.removeIf(o -> o.orderId().equals(orderId));
        if (level.isEmpty()) side.remove(order.price());
        return removed;
    }

    // ---------------------------------------------------------------
    //  QUERIES
    // ---------------------------------------------------------------

    private long availableQuantityAtOrBetter(Order aggressor) {
        TreeMap<BigDecimal, PriorityQueue<Order>> oppositeSide =
                aggressor.isBuy() ? asks : bids;
        long total = 0;
        for (Map.Entry<BigDecimal, PriorityQueue<Order>> entry : oppositeSide.entrySet()) {
            BigDecimal levelPrice = entry.getKey();
            boolean crosses = aggressor.isBuy()
                    ? aggressor.price() == null || aggressor.price().compareTo(levelPrice) >= 0
                    : aggressor.price() == null || aggressor.price().compareTo(levelPrice) <= 0;
            if (!crosses) break;
            for (Order o : entry.getValue()) total += o.remainingQuantity();
        }
        return total;
    }

    public Optional<BigDecimal> bestBid() {
        return bids.isEmpty() ? Optional.empty() : Optional.of(bids.firstKey());
    }

    public Optional<BigDecimal> bestAsk() {
        return asks.isEmpty() ? Optional.empty() : Optional.of(asks.firstKey());
    }

    public Optional<BigDecimal> spread() {
        if (bids.isEmpty() || asks.isEmpty()) return Optional.empty();
        return Optional.of(asks.firstKey().subtract(bids.firstKey()));
    }

    public Optional<BigDecimal> lastTradePrice() {
        return Optional.ofNullable(lastTradePrice);
    }

    public int bidLevels() { return bids.size(); }
    public int askLevels() { return asks.size(); }
    public int totalOrders() { return orderIndex.size(); }
    public String symbol()   { return symbol; }

    /** Snapshot of top N bid/ask levels for WebSocket market data feed */
    public OrderBookSnapshot snapshot(int depth) {
        List<PriceLevel> bidLevels = new ArrayList<>();
        List<PriceLevel> askLevels = new ArrayList<>();

        int count = 0;
        for (Map.Entry<BigDecimal, PriorityQueue<Order>> e : bids.entrySet()) {
            if (count++ >= depth) break;
            long qty = e.getValue().stream().mapToLong(Order::remainingQuantity).sum();
            bidLevels.add(new PriceLevel(e.getKey(), qty, e.getValue().size()));
        }
        count = 0;
        for (Map.Entry<BigDecimal, PriorityQueue<Order>> e : asks.entrySet()) {
            if (count++ >= depth) break;
            long qty = e.getValue().stream().mapToLong(Order::remainingQuantity).sum();
            askLevels.add(new PriceLevel(e.getKey(), qty, e.getValue().size()));
        }
        return new OrderBookSnapshot(symbol, bidLevels, askLevels, lastTradePrice);
    }

    public record PriceLevel(BigDecimal price, long totalQuantity, int orderCount) {}
    public record OrderBookSnapshot(String symbol, List<PriceLevel> bids, List<PriceLevel> asks, BigDecimal lastTradePrice) {}
}
