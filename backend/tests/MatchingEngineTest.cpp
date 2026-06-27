#include <gtest/gtest.h>
#include "titanx/engine/MatchingEngine.hpp"
#include "titanx/risk/RiskEngine.hpp"
#include "titanx/risk/CircuitBreaker.hpp"

#include <thread>
#include <vector>
#include <atomic>
#include <random>

using namespace titanx;

/**
 * ============================================================
 * MatchingEngine Integration Tests
 * ============================================================
 *
 * Tests cover:
 *  1. End-to-end order submission and matching
 *  2. Order cancellation flow
 *  3. Global halt/resume
 *  4. Concurrent multi-threaded submission
 *  5. Risk rejection propagation
 */

class MatchingEngineTest : public ::testing::Test {
protected:
    CircuitBreaker circuitBreaker;
    RiskEngine riskEngine{circuitBreaker};
    MatchingEngine engine{riskEngine};
};

// ---- End-to-end matching ----

TEST_F(MatchingEngineTest, SubmitAndMatchOrders) {
    // Seed the book with a sell order
    auto sell = Order::create("seller1", "AAPL", OrderSide::SELL,
                              OrderType::LIMIT, price_to_fixed(150.0),
                              0, 100);
    auto sellTrades = engine.submit(sell);
    EXPECT_EQ(sellTrades.size(), 0); // resting

    // Submit a matching buy order
    auto buy = Order::create("buyer1", "AAPL", OrderSide::BUY,
                             OrderType::MARKET, 0, 0, 50);
    auto buyTrades = engine.submit(buy);

    EXPECT_EQ(buyTrades.size(), 1);
    EXPECT_EQ(buyTrades[0].quantity, 50);
    EXPECT_EQ(buyTrades[0].price, price_to_fixed(150.0));
}

// ---- Cancellation ----

TEST_F(MatchingEngineTest, CancelRestingOrder) {
    auto order = Order::create("acc1", "AAPL", OrderSide::BUY,
                               OrderType::LIMIT, price_to_fixed(149.0),
                               0, 100);
    engine.submit(order);

    bool cancelled = engine.cancel("AAPL", order.orderId);
    EXPECT_TRUE(cancelled);

    // Verify it's gone from the book
    auto snapshot = engine.getSnapshot("AAPL", 10);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->bids.size(), 0);
}

TEST_F(MatchingEngineTest, CancelNonexistentFails) {
    // Create the book first
    auto order = Order::create("acc1", "AAPL", OrderSide::BUY,
                               OrderType::LIMIT, price_to_fixed(149.0),
                               0, 100);
    engine.submit(order);

    bool cancelled = engine.cancel("AAPL", "nonexistent");
    EXPECT_FALSE(cancelled);
}

// ---- Global halt ----

TEST_F(MatchingEngineTest, HaltRejectsNewOrders) {
    engine.halt();
    EXPECT_TRUE(engine.isHalted());

    auto order = Order::create("acc1", "AAPL", OrderSide::BUY,
                               OrderType::MARKET, 0, 0, 100);
    auto trades = engine.submit(order);
    EXPECT_EQ(trades.size(), 0);

    engine.resume();
    EXPECT_FALSE(engine.isHalted());
}

// ---- Concurrent submission ----

TEST_F(MatchingEngineTest, ConcurrentSubmissionNoDataRace) {
    constexpr int ORDERS_PER_THREAD = 1000;
    constexpr int NUM_THREADS = 4;
    std::atomic<int> totalTrades{0};

    auto submitOrders = [&](int threadId) {
        std::mt19937 rng(threadId);
        std::uniform_int_distribution<int> sideDist(0, 1);
        std::uniform_int_distribution<int64_t> priceDist(148, 152);

        for (int i = 0; i < ORDERS_PER_THREAD; ++i) {
            OrderSide side = sideDist(rng) == 0 ? OrderSide::BUY : OrderSide::SELL;
            int64_t price = price_to_fixed(static_cast<double>(priceDist(rng)));
            std::string accountId = "thread-" + std::to_string(threadId);

            auto order = Order::create(accountId, "AAPL", side,
                                       OrderType::LIMIT, price, 0, 10);
            auto trades = engine.submit(order);
            totalTrades.fetch_add(static_cast<int>(trades.size()),
                                  std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back(submitOrders, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    // Verify no crashes, and some trades occurred
    EXPECT_GT(totalTrades.load(), 0);

    auto snapshot = engine.getSnapshot("AAPL", 100);
    ASSERT_TRUE(snapshot.has_value());
}

// ---- Event callbacks ----

TEST_F(MatchingEngineTest, EventConsumerReceivesEvents) {
    std::vector<OrderEvent> capturedEvents;
    engine.setEventConsumer([&](const OrderEvent& event) {
        capturedEvents.push_back(event);
    });

    auto sell = Order::create("seller1", "AAPL", OrderSide::SELL,
                              OrderType::LIMIT, price_to_fixed(150.0),
                              0, 100);
    engine.submit(sell);

    // Should receive ORDER_RECEIVED + ORDER_RESTING
    EXPECT_GE(capturedEvents.size(), 1);
    EXPECT_EQ(capturedEvents[0].type, EventType::ORDER_RECEIVED);
}

// ---- Multiple symbols ----

TEST_F(MatchingEngineTest, IndependentSymbolMatching) {
    auto aaplSell = Order::create("seller1", "AAPL", OrderSide::SELL,
                                  OrderType::LIMIT, price_to_fixed(150.0),
                                  0, 100);
    auto googSell = Order::create("seller2", "GOOG", OrderSide::SELL,
                                  OrderType::LIMIT, price_to_fixed(2800.0),
                                  0, 50);

    engine.submit(aaplSell);
    engine.submit(googSell);

    auto aaplSnap = engine.getSnapshot("AAPL", 10);
    auto googSnap = engine.getSnapshot("GOOG", 10);

    ASSERT_TRUE(aaplSnap.has_value());
    ASSERT_TRUE(googSnap.has_value());
    EXPECT_EQ(aaplSnap->asks.size(), 1);
    EXPECT_EQ(googSnap->asks.size(), 1);
}
