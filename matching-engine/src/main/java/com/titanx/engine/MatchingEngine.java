package com.titanx.engine;

import com.titanx.event.FillEvent;
import com.titanx.event.OrderEvent;
import com.titanx.event.EventType;
import com.titanx.model.*;
import com.titanx.risk.RiskEngine;
import com.titanx.risk.RiskResult;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.locks.*;
import java.util.function.Consumer;

/**
 * ============================================================
 * TitanX MatchingEngine — Phase 1 + Concurrency (Phase 2)
 * ============================================================
 *
 * Concurrency Model:
 *   "I use a ReentrantReadWriteLock per symbol so different instruments
 *    can match concurrently. All order operations that modify the book
 *    acquire the WRITE lock; read queries (market data) use READ lock
 *    allowing concurrent reads from multiple WebSocket subscribers."
 *
 *    The next evolution is a Disruptor-based ring buffer per symbol —
 *    that eliminates the lock entirely via single-threaded ring consumption,
 *    achieving sub-microsecond latency as LMAX demonstrated.
 *
 * Stop-limit trigger:
 *   Stop-limit orders are held in a ConcurrentHashMap keyed by symbol.
 *   After each trade, we check if lastTradePrice crossed any stop triggers.
 *
 * Event sourcing:
 *   Every state change emits an OrderEvent to the eventConsumer.
 *   The consumer (EventStore) persists this to PostgreSQL asynchronously.
 */
public class MatchingEngine {

    private static final Logger log = LoggerFactory.getLogger(MatchingEngine.class);

    // One OrderBook per symbol
    private final ConcurrentHashMap<String, OrderBook> books = new ConcurrentHashMap<>();

    // One lock per symbol — instruments match independently
    private final ConcurrentHashMap<String, ReentrantReadWriteLock> locks = new ConcurrentHashMap<>();

    // Stop-limit pending orders: symbol → list of pending stop orders
    private final ConcurrentHashMap<String, List<Order>> stopOrders = new ConcurrentHashMap<>();

    private final RiskEngine riskEngine;
    private Consumer<OrderEvent> eventConsumer = event -> {}; // default no-op
    private Consumer<FillEvent> fillConsumer   = fill  -> {}; // default no-op

    // ---- Global kill switch (Phase 4 circuit breaker) ----
    private volatile boolean halted = false;

    public MatchingEngine(RiskEngine riskEngine) {
        this.riskEngine = riskEngine;
    }

    // ---------------------------------------------------------------
    //  ORDER SUBMISSION (main entry point)
    // ---------------------------------------------------------------

    /**
     * Submit an order for matching.
     *
     * Steps:
     *   1. Global halt check
     *   2. Pre-trade risk checks
     *   3. Acquire per-symbol write lock
     *   4. Match in the order book
     *   5. Process stop-limit triggers
     *   6. Emit events
     *
     * Returns: list of trades generated (may be empty for resting limit orders)
     */
    public List<Trade> submit(Order order) {
        if (halted) {
            log.warn("Engine halted — rejecting order {}", order.orderId());
            emit(OrderEvent.rejected(order, "ENGINE_HALTED"));
            return List.of();
        }

        // Phase 2: Pre-trade risk checks
        RiskResult riskResult = riskEngine.checkPreTrade(order);
        if (!riskResult.approved()) {
            log.info("Order {} rejected by risk: {}", order.orderId(), riskResult.reason());
            emit(OrderEvent.rejected(order, riskResult.reason()));
            return List.of();
        }

        OrderBook book = books.computeIfAbsent(order.symbol(), OrderBook::new);
        ReentrantReadWriteLock lock = locks.computeIfAbsent(order.symbol(),
                s -> new ReentrantReadWriteLock());

        emit(OrderEvent.received(order));
        long startNanos = System.nanoTime();

        lock.writeLock().lock();
        List<Trade> trades;
        try {
            trades = book.match(order);
        } finally {
            lock.writeLock().unlock();
        }

        long latencyMicros = (System.nanoTime() - startNanos) / 1_000;
        log.info("Matched order {} in {}µs, {} trades generated",
                order.orderId(), latencyMicros, trades.size());

        // Emit fill events
        for (Trade trade : trades) {
            FillEvent fill = new FillEvent(trade, latencyMicros);
            fillConsumer.accept(fill);
            emit(OrderEvent.filled(order, trade));
        }

        // Check stop-limit triggers after each trade
        if (!trades.isEmpty()) {
            checkStopTriggers(order.symbol(), book);
        }

        // Emit resting confirmation if order is now in the book
        book.lastTradePrice().ifPresent(p -> {
            if (order.type() == com.titanx.model.OrderType.LIMIT && trades.isEmpty()) {
                emit(OrderEvent.resting(order));
            }
        });

        return trades;
    }

    /**
     * Cancel a resting order.
     * Must acquire write lock — we're modifying the book.
     */
    public boolean cancel(String symbol, String orderId) {
        OrderBook book = books.get(symbol);
        if (book == null) return false;

        ReentrantReadWriteLock lock = locks.get(symbol);
        if (lock == null) return false;

        lock.writeLock().lock();
        try {
            boolean removed = book.removeFromBook(orderId);
            if (removed) log.info("Cancelled order {} on {}", orderId, symbol);
            return removed;
        } finally {
            lock.writeLock().unlock();
        }
    }

    // ---------------------------------------------------------------
    //  STOP-LIMIT TRIGGER PROCESSING
    // ---------------------------------------------------------------

    private void checkStopTriggers(String symbol, OrderBook book) {
        List<Order> pending = stopOrders.getOrDefault(symbol, List.of());
        if (pending.isEmpty()) return;

        book.lastTradePrice().ifPresent(lastPrice -> {
            List<Order> toActivate = new ArrayList<>();
            pending.removeIf(stop -> {
                boolean triggered = stop.isBuy()
                        ? lastPrice.compareTo(stop.stopPrice()) >= 0
                        : lastPrice.compareTo(stop.stopPrice()) <= 0;
                if (triggered) toActivate.add(stop);
                return triggered;
            });
            // Re-submit triggered stops as LIMIT orders
            for (Order stop : toActivate) {
                Order limitOrder = Order.create(stop.accountId(), stop.symbol(),
                        stop.side(), com.titanx.model.OrderType.LIMIT,
                        stop.price(), null, stop.remainingQuantity());
                log.info("Stop order {} triggered at {}, submitting as LIMIT", stop.orderId(), lastPrice);
                submit(limitOrder);
            }
        });
    }

    // ---------------------------------------------------------------
    //  READ QUERIES (uses READ lock — allows concurrent market data reads)
    // ---------------------------------------------------------------

    public Optional<OrderBook.OrderBookSnapshot> getSnapshot(String symbol, int depth) {
        OrderBook book = books.get(symbol);
        if (book == null) return Optional.empty();

        ReentrantReadWriteLock lock = locks.get(symbol);
        lock.readLock().lock();
        try {
            return Optional.of(book.snapshot(depth));
        } finally {
            lock.readLock().unlock();
        }
    }

    // ---------------------------------------------------------------
    //  CONTROL
    // ---------------------------------------------------------------

    public void halt()   { this.halted = true;  log.warn("ENGINE HALTED — global kill switch activated"); }
    public void resume() { this.halted = false; log.info("Engine resumed"); }
    public boolean isHalted() { return halted; }

    public void setEventConsumer(Consumer<OrderEvent> consumer) { this.eventConsumer = consumer; }
    public void setFillConsumer(Consumer<FillEvent> consumer)   { this.fillConsumer = consumer; }

    private void emit(OrderEvent event) {
        try { eventConsumer.accept(event); }
        catch (Exception e) { log.error("Event consumer error", e); }
    }

    public Map<String, OrderBook> getBooks() { return Collections.unmodifiableMap(books); }
}
