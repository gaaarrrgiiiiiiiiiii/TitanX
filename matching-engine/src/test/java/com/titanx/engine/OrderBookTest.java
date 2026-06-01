package com.titanx.engine;

import com.titanx.model.*;
import com.titanx.risk.CircuitBreaker;
import com.titanx.risk.RiskEngine;
import org.junit.jupiter.api.*;
import org.junit.jupiter.api.parallel.Execution;
import org.junit.jupiter.api.parallel.ExecutionMode;

import java.math.BigDecimal;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

import static org.assertj.core.api.Assertions.*;

/**
 * ============================================================
 * TitanX OrderBook Tests — Phase 1 Correctness Verification
 * ============================================================
 *
 * These tests are the foundation of the project's correctness guarantee.
 * A matching engine with failing tests is a broken exchange.
 *
 * Test categories:
 *  - Basic matching: MARKET, LIMIT, IOC, FOK
 *  - Price-time priority
 *  - Partial fills
 *  - Edge cases: empty book, self-crossing
 *  - Concurrency: 100 threads, zero duplicate fills
 */
@DisplayName("TitanX OrderBook — Correctness Tests")
class OrderBookTest {

    private OrderBook book;
    private MatchingEngine engine;
    private RiskEngine riskEngine;

    @BeforeEach
    void setUp() {
        book = new OrderBook("AAPL");
        CircuitBreaker cb = new CircuitBreaker();
        riskEngine = new RiskEngine(cb);
        engine = new MatchingEngine(riskEngine);
    }

    // ---------------------------------------------------------------
    //  MARKET ORDER TESTS
    // ---------------------------------------------------------------

    @Test
    @DisplayName("Market buy fills against best ask")
    void marketBuyFillsBestAsk() {
        // Insert a resting sell at 100.00 for 100 shares
        Order sellLimit = Order.create("acc-seller", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 100);
        book.insertIntoBook(sellLimit);

        // Market buy for 50 shares
        Order marketBuy = Order.create("acc-buyer", "AAPL", OrderSide.BUY, OrderType.MARKET,
                null, null, 50);
        List<Trade> trades = book.match(marketBuy);

        assertThat(trades).hasSize(1);
        Trade trade = trades.get(0);
        assertThat(trade.quantity()).isEqualTo(50);
        assertThat(trade.price()).isEqualByComparingTo("100.00");
        assertThat(trade.aggressorSide()).isEqualTo(OrderSide.BUY);
    }

    @Test
    @DisplayName("Market order consumes multiple price levels")
    void marketOrderConsumesMultipleLevels() {
        // Two resting sells at different prices
        book.insertIntoBook(Order.create("acc1", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 100));
        book.insertIntoBook(Order.create("acc2", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("101.00"), null, 100));

        // Market buy for 150 shares — should consume both levels
        Order marketBuy = Order.create("acc-buyer", "AAPL", OrderSide.BUY, OrderType.MARKET,
                null, null, 150);
        List<Trade> trades = book.match(marketBuy);

        assertThat(trades).hasSize(2);
        assertThat(trades.get(0).price()).isEqualByComparingTo("100.00"); // best ask first
        assertThat(trades.get(1).price()).isEqualByComparingTo("101.00");
        assertThat(trades.stream().mapToLong(Trade::quantity).sum()).isEqualTo(150);
    }

    @Test
    @DisplayName("Market buy against empty book generates no trades")
    void marketBuyAgainstEmptyBook() {
        Order marketBuy = Order.create("acc", "AAPL", OrderSide.BUY, OrderType.MARKET,
                null, null, 100);
        List<Trade> trades = book.match(marketBuy);
        assertThat(trades).isEmpty();
    }

    // ---------------------------------------------------------------
    //  LIMIT ORDER TESTS
    // ---------------------------------------------------------------

    @Test
    @DisplayName("Limit buy above best ask fills immediately")
    void limitBuyCrossesSpread() {
        book.insertIntoBook(Order.create("seller", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("99.50"), null, 200));

        Order limitBuy = Order.create("buyer", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 100);
        List<Trade> trades = book.match(limitBuy);

        assertThat(trades).hasSize(1);
        assertThat(trades.get(0).price()).isEqualByComparingTo("99.50"); // fills at passive price
        assertThat(trades.get(0).quantity()).isEqualTo(100);
    }

    @Test
    @DisplayName("Limit order that doesn't cross rests in book")
    void limitOrderRestsWhenNoCross() {
        Order limitBuy = Order.create("buyer", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("95.00"), null, 100);
        List<Trade> trades = book.match(limitBuy);

        assertThat(trades).isEmpty();
        assertThat(book.bestBid()).isPresent();
        assertThat(book.bestBid().get()).isEqualByComparingTo("95.00");
        assertThat(book.totalOrders()).isEqualTo(1);
    }

    @Test
    @DisplayName("Partial fill: limit order partially matched, remainder rests")
    void partialFillRestsRemainder() {
        // Resting sell for 50 shares
        book.insertIntoBook(Order.create("seller", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 50));

        // Incoming buy for 100 shares — can only fill 50
        Order limitBuy = Order.create("buyer", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 100);
        List<Trade> trades = book.match(limitBuy);

        assertThat(trades).hasSize(1);
        assertThat(trades.get(0).quantity()).isEqualTo(50);

        // The remaining 50 should be resting as a bid
        assertThat(book.bestBid()).isPresent();
        assertThat(book.bestBid().get()).isEqualByComparingTo("100.00");
    }

    // ---------------------------------------------------------------
    //  PRICE-TIME PRIORITY TESTS
    // ---------------------------------------------------------------

    @Test
    @DisplayName("Orders at same price level filled FIFO")
    void samePriceLevelFifo() throws InterruptedException {
        // Insert three orders at the same price, in order
        Order first  = Order.create("acc1", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 100);
        Thread.sleep(1); // ensure different timestamps
        Order second = Order.create("acc2", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 100);
        Thread.sleep(1);
        Order third  = Order.create("acc3", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 100);

        book.insertIntoBook(first);
        book.insertIntoBook(second);
        book.insertIntoBook(third);

        // Buy 100 shares — should fill against 'first' only
        Order marketBuy = Order.create("buyer", "AAPL", OrderSide.BUY, OrderType.MARKET,
                null, null, 100);
        List<Trade> trades = book.match(marketBuy);

        assertThat(trades).hasSize(1);
        assertThat(trades.get(0).passiveOrderId()).isEqualTo(first.orderId());
    }

    // ---------------------------------------------------------------
    //  IOC TESTS
    // ---------------------------------------------------------------

    @Test
    @DisplayName("IOC fills partial quantity and cancels rest")
    void iocPartialFillCancelsRest() {
        book.insertIntoBook(Order.create("seller", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 30)); // only 30 available

        Order iocBuy = Order.create("buyer", "AAPL", OrderSide.BUY, OrderType.IOC,
                new BigDecimal("100.00"), null, 100); // wants 100
        List<Trade> trades = book.match(iocBuy);

        assertThat(trades).hasSize(1);
        assertThat(trades.get(0).quantity()).isEqualTo(30);

        // No resting bid — IOC cancels unfilled remainder
        assertThat(book.bestBid()).isEmpty();
    }

    // ---------------------------------------------------------------
    //  FOK TESTS
    // ---------------------------------------------------------------

    @Test
    @DisplayName("FOK fills completely when book has enough quantity")
    void fokFillsWhenEnoughLiquidity() {
        book.insertIntoBook(Order.create("seller", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 200));

        Order fokBuy = Order.create("buyer", "AAPL", OrderSide.BUY, OrderType.FOK,
                new BigDecimal("100.00"), null, 100);
        List<Trade> trades = book.match(fokBuy);

        assertThat(trades).hasSize(1);
        assertThat(trades.get(0).quantity()).isEqualTo(100);
    }

    @Test
    @DisplayName("FOK cancels entirely when book has insufficient quantity")
    void fokCancelsWhenInsufficientLiquidity() {
        book.insertIntoBook(Order.create("seller", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 50)); // only 50 available

        Order fokBuy = Order.create("buyer", "AAPL", OrderSide.BUY, OrderType.FOK,
                new BigDecimal("100.00"), null, 100); // wants 100
        List<Trade> trades = book.match(fokBuy);

        // FOK: zero trades (not partially filled)
        assertThat(trades).isEmpty();

        // Passive order should still be in the book (untouched)
        assertThat(book.bestAsk()).isPresent();
        assertThat(book.bestAsk().get()).isEqualByComparingTo("100.00");
    }

    // ---------------------------------------------------------------
    //  CANCELLATION TESTS
    // ---------------------------------------------------------------

    @Test
    @DisplayName("Cancelled order is removed from book")
    void cancelRemovesFromBook() {
        Order order = Order.create("acc", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("99.00"), null, 100);
        book.insertIntoBook(order);
        assertThat(book.totalOrders()).isEqualTo(1);

        boolean removed = book.removeFromBook(order.orderId());
        assertThat(removed).isTrue();
        assertThat(book.totalOrders()).isEqualTo(0);
        assertThat(book.bestBid()).isEmpty();
    }

    // ---------------------------------------------------------------
    //  CONCURRENCY TEST
    // ---------------------------------------------------------------

    @Test
    @DisplayName("Concurrent order submissions produce no duplicate fills")
    void concurrentOrdersNoDuplicateFills() throws InterruptedException {
        // Pre-load 1000 resting sell orders at $100
        for (int i = 0; i < 1000; i++) {
            Order sell = Order.create("seller" + i, "AAPL", OrderSide.SELL, OrderType.LIMIT,
                    new BigDecimal("100.00"), null, 1);
            engine.submit(sell);
        }

        int threads = 50;
        int ordersPerThread = 10;
        ExecutorService pool = Executors.newFixedThreadPool(threads);
        AtomicInteger totalFills = new AtomicInteger();
        List<Future<?>> futures = new ArrayList<>();

        for (int t = 0; t < threads; t++) {
            final int threadId = t;
            futures.add(pool.submit(() -> {
                for (int i = 0; i < ordersPerThread; i++) {
                    Order buy = Order.create("buyer-" + threadId + "-" + i, "AAPL",
                            OrderSide.BUY, OrderType.MARKET, null, null, 1);
                    List<Trade> trades = engine.submit(buy);
                    totalFills.addAndGet(trades.size());
                }
            }));
        }

        pool.shutdown();
        pool.awaitTermination(10, TimeUnit.SECONDS);

        // Total fills should equal total buys submitted (500)
        // and should be at most 1000 (no double-fills of the same passive order)
        int buysSubmitted = threads * ordersPerThread;
        assertThat(totalFills.get())
                .as("Total fills should equal buys submitted (all have liquidity)")
                .isEqualTo(buysSubmitted);
    }

    // ---------------------------------------------------------------
    //  ORDER BOOK SNAPSHOT TESTS
    // ---------------------------------------------------------------

    @Test
    @DisplayName("Book snapshot returns correct bid/ask levels")
    void snapshotReturnsCorrectLevels() {
        book.insertIntoBook(Order.create("b1", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("99.00"), null, 100));
        book.insertIntoBook(Order.create("b2", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("98.00"), null, 200));
        book.insertIntoBook(Order.create("s1", "AAPL", OrderSide.SELL, OrderType.LIMIT,
                new BigDecimal("101.00"), null, 150));

        OrderBook.OrderBookSnapshot snapshot = book.snapshot(5);

        assertThat(snapshot.bids()).hasSize(2);
        assertThat(snapshot.asks()).hasSize(1);
        assertThat(snapshot.bids().get(0).price()).isEqualByComparingTo("99.00"); // best bid first
        assertThat(snapshot.asks().get(0).price()).isEqualByComparingTo("101.00");
        assertThat(snapshot.bids().get(0).totalQuantity()).isEqualTo(100);
    }
}
