package com.titanx.simulation;

import com.titanx.engine.MatchingEngine;
import com.titanx.model.*;
import com.titanx.risk.CircuitBreaker;
import com.titanx.risk.RiskEngine;

import java.math.BigDecimal;
import java.math.RoundingMode;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.random.RandomGenerator;

/**
 * ============================================================
 * TitanX OrderGenerator — Load Testing Simulation
 * ============================================================
 *
 * Simulates realistic order flow to measure throughput and latency.
 *
 * Scenario: 10 market makers + 90 retail traders, all sending orders
 * to a single AAPL order book.
 *
 * Output:
 *   - Throughput: orders/second
 *   - Latency: p50, p95, p99 in microseconds
 *   - Fill rate: % of orders that resulted in at least one trade
 *   - Risk rejection rate
 *
 * Run: java -cp titanx-engine.jar com.titanx.simulation.OrderGenerator
 *
 * Target: 100,000 orders/minute (1,667/second) on commodity hardware.
 */
public class OrderGenerator {

    private static final String SYMBOL = "AAPL";
    private static final int SIMULATION_SECONDS = 30;
    private static final int THREADS = 20;
    private static final int ORDERS_PER_THREAD_PER_SECOND = 50;

    // Simulated market makers seed the book with resting orders
    private static final int MARKET_MAKER_ACCOUNTS = 10;

    public static void main(String[] args) throws InterruptedException {
        System.out.println("=================================================");
        System.out.println("  TitanX Load Simulation — Goldman Sachs Interview");
        System.out.println("=================================================");
        System.out.printf("  Threads: %d | Duration: %ds | Target: ~%d orders/sec%n%n",
                THREADS, SIMULATION_SECONDS, THREADS * ORDERS_PER_THREAD_PER_SECOND);

        CircuitBreaker cb = new CircuitBreaker();
        RiskEngine riskEngine = new RiskEngine(cb);
        MatchingEngine engine = new MatchingEngine(riskEngine);

        // Seed the order book with market maker quotes
        seedMarketMakerOrders(engine);

        // Metrics
        AtomicLong totalOrders  = new AtomicLong();
        AtomicLong totalFills   = new AtomicLong();
        AtomicLong totalLatency = new AtomicLong();
        List<Long> latencySamples = Collections.synchronizedList(new ArrayList<>());

        ExecutorService pool = Executors.newFixedThreadPool(THREADS);
        long startTime = System.currentTimeMillis();

        for (int t = 0; t < THREADS; t++) {
            final int threadId = t;
            pool.submit(() -> runTraderThread(threadId, engine, SIMULATION_SECONDS,
                    ORDERS_PER_THREAD_PER_SECOND, totalOrders, totalFills, latencySamples));
        }

        pool.shutdown();
        pool.awaitTermination(SIMULATION_SECONDS + 5, TimeUnit.SECONDS);

        long elapsed = System.currentTimeMillis() - startTime;
        printResults(totalOrders.get(), totalFills.get(), elapsed, latencySamples);
    }

    private static void seedMarketMakerOrders(MatchingEngine engine) {
        RandomGenerator rng = RandomGenerator.getDefault();
        BigDecimal midPrice = new BigDecimal("150.00");

        System.out.println("Seeding market maker orders...");
        for (int mm = 0; mm < MARKET_MAKER_ACCOUNTS; mm++) {
            String mmId = "mm-" + mm;
            // Quote 5 bid levels and 5 ask levels
            for (int level = 1; level <= 5; level++) {
                BigDecimal bidPrice = midPrice.subtract(
                        new BigDecimal(level * 10).movePointLeft(2));
                BigDecimal askPrice = midPrice.add(
                        new BigDecimal(level * 10).movePointLeft(2));

                // Market maker bids
                engine.submit(Order.create(mmId, SYMBOL, OrderSide.BUY, OrderType.LIMIT,
                        bidPrice, null, 100 + rng.nextInt(400)));
                // Market maker asks
                engine.submit(Order.create(mmId, SYMBOL, OrderSide.SELL, OrderType.LIMIT,
                        askPrice, null, 100 + rng.nextInt(400)));
            }
        }
        System.out.println("Book seeded with " + (MARKET_MAKER_ACCOUNTS * 10) + " resting orders\n");
    }

    private static void runTraderThread(int threadId, MatchingEngine engine, int durationSeconds,
                                        int ordersPerSecond, AtomicLong totalOrders,
                                        AtomicLong totalFills, List<Long> latencySamples) {
        RandomGenerator rng = RandomGenerator.getDefault();
        long endTime = System.currentTimeMillis() + (durationSeconds * 1000L);
        long intervalNs = 1_000_000_000L / ordersPerSecond;

        while (System.currentTimeMillis() < endTime) {
            long loopStart = System.nanoTime();

            Order order = generateRandomOrder(threadId, rng);
            long matchStart = System.nanoTime();
            List<com.titanx.model.Trade> trades = engine.submit(order);
            long latencyMicros = (System.nanoTime() - matchStart) / 1_000;

            totalOrders.incrementAndGet();
            totalFills.addAndGet(trades.size());
            latencySamples.add(latencyMicros);

            // Busy-spin to hit target rate (accurate for benchmarking)
            long elapsed = System.nanoTime() - loopStart;
            if (elapsed < intervalNs) {
                long sleepNs = intervalNs - elapsed;
                try { Thread.sleep(sleepNs / 1_000_000, (int)(sleepNs % 1_000_000)); }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); return; }
            }
        }
    }

    private static Order generateRandomOrder(int threadId, RandomGenerator rng) {
        String accountId = "trader-" + threadId;
        boolean isBuy = rng.nextBoolean();
        OrderSide side = isBuy ? OrderSide.BUY : OrderSide.SELL;

        // Mix of order types: 60% limit, 30% market, 10% IOC
        int typeRoll = rng.nextInt(10);
        OrderType type;
        BigDecimal price;
        if (typeRoll < 6) {
            type = OrderType.LIMIT;
            // Price within 1% of mid
            double deviation = (rng.nextDouble() - 0.5) * 2.0; // -1% to +1%
            price = new BigDecimal(150.0 + deviation)
                    .setScale(2, RoundingMode.HALF_UP);
        } else if (typeRoll < 9) {
            type = OrderType.MARKET;
            price = null;
        } else {
            type = OrderType.IOC;
            price = new BigDecimal("150.00");
        }

        long quantity = 1 + rng.nextLong(99); // 1-100 shares

        return Order.create(accountId, SYMBOL, side, type, price, null, quantity);
    }

    private static void printResults(long totalOrders, long totalFills, long elapsedMs,
                                     List<Long> latencySamples) {
        double throughput = totalOrders * 1000.0 / elapsedMs;
        double fillRate = totalOrders > 0 ? (totalFills * 100.0 / totalOrders) : 0;

        List<Long> sorted = new ArrayList<>(latencySamples);
        Collections.sort(sorted);

        System.out.println("\n=================================================");
        System.out.println("  TitanX Simulation Results");
        System.out.println("=================================================");
        System.out.printf("  Total Orders:    %,d%n", totalOrders);
        System.out.printf("  Total Fills:     %,d%n", totalFills);
        System.out.printf("  Throughput:      %,.1f orders/sec%n", throughput);
        System.out.printf("  Fill Rate:       %.1f%%%n", fillRate);

        if (!sorted.isEmpty()) {
            System.out.println("\n  Matching Latency (µs):");
            System.out.printf("    p50  = %,d µs%n", percentile(sorted, 50));
            System.out.printf("    p95  = %,d µs%n", percentile(sorted, 95));
            System.out.printf("    p99  = %,d µs%n", percentile(sorted, 99));
            System.out.printf("    max  = %,d µs%n", sorted.get(sorted.size() - 1));
        }

        System.out.println("\n=================================================");
        System.out.println("  Interview Talking Points:");
        System.out.println("  - Lock contention: ReentrantReadWriteLock per symbol");
        System.out.println("  - Next step: Disruptor ring buffer for sub-µs latency");
        System.out.println("  - Persistence: async EventStore never blocks hot path");
        System.out.println("=================================================");
    }

    private static long percentile(List<Long> sorted, int pct) {
        int index = (int) Math.ceil(pct / 100.0 * sorted.size()) - 1;
        return sorted.get(Math.max(0, index));
    }
}
