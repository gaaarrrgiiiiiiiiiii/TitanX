#include <gtest/gtest.h>
#include "titanx/risk/RiskEngine.hpp"
#include "titanx/risk/CircuitBreaker.hpp"

using namespace titanx;

/**
 * ============================================================
 * RiskEngine Unit Tests
 * ============================================================
 *
 * Tests cover all 6 pre-trade checks:
 *  1. Circuit breaker halt
 *  2. Duplicate order ID
 *  3. Notional limit ($5M)
 *  4. Position limit (10K shares)
 *  5. Fat-finger protection (10% deviation)
 *  6. Self-trade prevention
 */

class RiskEngineTest : public ::testing::Test {
protected:
    CircuitBreaker circuitBreaker;
    RiskEngine riskEngine{circuitBreaker};
};

// ---- 1. Circuit breaker ----

TEST_F(RiskEngineTest, RejectsWhenCircuitBreakerTriggered) {
    circuitBreaker.haltSymbol("AAPL", "test");

    auto order = Order::create("acc1", "AAPL", OrderSide::BUY,
                               OrderType::LIMIT, price_to_fixed(150.0),
                               0, 100);
    auto result = riskEngine.checkPreTrade(order);

    EXPECT_FALSE(result.approved);
    EXPECT_EQ(result.reason, "CIRCUIT_BREAKER_HALT");
}

// ---- 2. Duplicate order ID ----

TEST_F(RiskEngineTest, RejectsDuplicateOrderId) {
    auto order = Order::create("acc1", "AAPL", OrderSide::BUY,
                               OrderType::LIMIT, price_to_fixed(150.0),
                               0, 100);

    auto result1 = riskEngine.checkPreTrade(order);
    EXPECT_TRUE(result1.approved);

    // Same order submitted again
    auto result2 = riskEngine.checkPreTrade(order);
    EXPECT_FALSE(result2.approved);
    EXPECT_EQ(result2.reason, "DUPLICATE_ORDER_ID");
}

// ---- 3. Notional limit ----

TEST_F(RiskEngineTest, RejectsExcessiveNotional) {
    // $10,000 × 600 shares = $6M notional > $5M limit
    auto order = Order::create("acc1", "AAPL", OrderSide::BUY,
                               OrderType::LIMIT, price_to_fixed(10000.0),
                               0, 600);
    auto result = riskEngine.checkPreTrade(order);

    EXPECT_FALSE(result.approved);
    EXPECT_EQ(result.reason, "NOTIONAL_LIMIT_EXCEEDED");
}

TEST_F(RiskEngineTest, AcceptsValidNotional) {
    // $150 × 100 shares = $15K << $5M
    auto order = Order::create("acc1", "AAPL", OrderSide::BUY,
                               OrderType::LIMIT, price_to_fixed(150.0),
                               0, 100);
    auto result = riskEngine.checkPreTrade(order);
    EXPECT_TRUE(result.approved);
}

// ---- 4. Position limit ----

TEST_F(RiskEngineTest, RejectsPositionExceedingLimit) {
    // Simulate holding 9,900 shares
    riskEngine.recordFill("acc1", "AAPL", OrderSide::BUY, 9900);

    // Trying to buy 200 more → 10,100 > 10,000 limit
    auto order = Order::create("acc1", "AAPL", OrderSide::BUY,
                               OrderType::LIMIT, price_to_fixed(150.0),
                               0, 200);
    auto result = riskEngine.checkPreTrade(order);

    EXPECT_FALSE(result.approved);
    EXPECT_EQ(result.reason, "POSITION_LIMIT_EXCEEDED");
}

// ---- 5. Fat-finger protection ----

TEST_F(RiskEngineTest, RejectsFatFingerPrice) {
    // Set last trade at $150
    riskEngine.updateLastTradePrice("AAPL", price_to_fixed(150.0));

    // Order at $200 = 33% deviation > 10% threshold
    auto order = Order::create("acc1", "AAPL", OrderSide::BUY,
                               OrderType::LIMIT, price_to_fixed(200.0),
                               0, 100);
    auto result = riskEngine.checkPreTrade(order);

    EXPECT_FALSE(result.approved);
    EXPECT_EQ(result.reason, "FAT_FINGER_PRICE_DEVIATION");
}

TEST_F(RiskEngineTest, AcceptsReasonablePriceDeviation) {
    riskEngine.updateLastTradePrice("AAPL", price_to_fixed(150.0));

    // Order at $155 = 3.3% deviation < 10% threshold
    auto order = Order::create("acc1", "AAPL", OrderSide::BUY,
                               OrderType::LIMIT, price_to_fixed(155.0),
                               0, 100);
    auto result = riskEngine.checkPreTrade(order);
    EXPECT_TRUE(result.approved);
}

// ---- 6. Self-trade prevention ----

TEST_F(RiskEngineTest, PreventsSelfTrade) {
    // Submit a buy order from acc1
    auto buyOrder = Order::create("acc1", "AAPL", OrderSide::BUY,
                                  OrderType::LIMIT, price_to_fixed(150.0),
                                  0, 100);
    auto result1 = riskEngine.checkPreTrade(buyOrder);
    EXPECT_TRUE(result1.approved);

    // Now try to submit a sell from same account
    auto sellOrder = Order::create("acc1", "AAPL", OrderSide::SELL,
                                   OrderType::LIMIT, price_to_fixed(155.0),
                                   0, 50);
    auto result2 = riskEngine.checkPreTrade(sellOrder);
    EXPECT_FALSE(result2.approved);
    EXPECT_EQ(result2.reason, "SELF_TRADE_PREVENTION");
}

// ---- Counters ----

TEST_F(RiskEngineTest, TracksCheckAndRejectCounters) {
    auto order1 = Order::create("acc1", "AAPL", OrderSide::BUY,
                                OrderType::LIMIT, price_to_fixed(150.0),
                                0, 100);
    riskEngine.checkPreTrade(order1);

    circuitBreaker.haltSymbol("AAPL", "test");
    auto order2 = Order::create("acc2", "AAPL", OrderSide::BUY,
                                OrderType::LIMIT, price_to_fixed(150.0),
                                0, 100);
    riskEngine.checkPreTrade(order2);

    EXPECT_EQ(riskEngine.totalChecked(), 2);
    EXPECT_EQ(riskEngine.totalRejected(), 1);
}
