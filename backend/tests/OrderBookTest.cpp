#include <gtest/gtest.h>
#include "titanx/engine/OrderBook.hpp"

using namespace titanx;

/**
 * ============================================================
 * OrderBook Unit Tests
 * ============================================================
 *
 * Tests cover:
 *  1. LIMIT order insertion and resting
 *  2. MARKET order matching against resting orders
 *  3. LIMIT order crossing (aggressor meets passive)
 *  4. Partial fills
 *  5. IOC (fill-or-cancel remainder)
 *  6. FOK (all-or-nothing)
 *  7. Cancel operations
 *  8. Book snapshot accuracy
 */

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book{"AAPL"};

    Order makeLimitBuy(int64_t priceDouble, int64_t qty) {
        return Order::create("acc1", "AAPL", OrderSide::BUY,
                             OrderType::LIMIT, price_to_fixed(priceDouble),
                             0, qty);
    }

    Order makeLimitSell(int64_t priceDouble, int64_t qty) {
        return Order::create("acc2", "AAPL", OrderSide::SELL,
                             OrderType::LIMIT, price_to_fixed(priceDouble),
                             0, qty);
    }

    Order makeMarketBuy(int64_t qty) {
        return Order::create("acc1", "AAPL", OrderSide::BUY,
                             OrderType::MARKET, 0, 0, qty);
    }

    Order makeMarketSell(int64_t qty) {
        return Order::create("acc2", "AAPL", OrderSide::SELL,
                             OrderType::MARKET, 0, 0, qty);
    }
};

// ---- LIMIT order resting (no match, should rest in book) ----

TEST_F(OrderBookTest, LimitBuyRestsWhenNoSellers) {
    auto order = makeLimitBuy(150, 100);
    auto trades = book.match(order);

    EXPECT_EQ(trades.size(), 0);
    EXPECT_EQ(book.bidLevels(), 1);
    EXPECT_EQ(book.totalOrders(), 1);
    EXPECT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(*book.bestBid(), price_to_fixed(150));
}

TEST_F(OrderBookTest, LimitSellRestsWhenNoBuyers) {
    auto order = makeLimitSell(155, 50);
    auto trades = book.match(order);

    EXPECT_EQ(trades.size(), 0);
    EXPECT_EQ(book.askLevels(), 1);
    EXPECT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(*book.bestAsk(), price_to_fixed(155));
}

// ---- MARKET order matching ----

TEST_F(OrderBookTest, MarketBuyMatchesAgainstAsks) {
    // Seed the book with sell orders
    book.match(makeLimitSell(150, 100));
    book.match(makeLimitSell(151, 50));

    auto marketBuy = makeMarketBuy(75);
    auto trades = book.match(marketBuy);

    EXPECT_EQ(trades.size(), 1);           // single fill at best ask
    EXPECT_EQ(trades[0].quantity, 75);
    EXPECT_EQ(trades[0].price, price_to_fixed(150));
}

TEST_F(OrderBookTest, MarketBuySweepsMultipleLevels) {
    book.match(makeLimitSell(150, 50));
    book.match(makeLimitSell(151, 50));

    auto marketBuy = makeMarketBuy(80);  // needs 50 at 150 + 30 at 151
    auto trades = book.match(marketBuy);

    EXPECT_EQ(trades.size(), 2);
    EXPECT_EQ(trades[0].quantity, 50);    // full fill at 150
    EXPECT_EQ(trades[0].price, price_to_fixed(150));
    EXPECT_EQ(trades[1].quantity, 30);    // partial fill at 151
    EXPECT_EQ(trades[1].price, price_to_fixed(151));
}

// ---- LIMIT order crossing ----

TEST_F(OrderBookTest, LimitBuyCrossesSpread) {
    book.match(makeLimitSell(150, 100));

    auto aggressiveBuy = Order::create("acc1", "AAPL", OrderSide::BUY,
                                       OrderType::LIMIT, price_to_fixed(150),
                                       0, 40);
    auto trades = book.match(aggressiveBuy);

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 40);
    EXPECT_EQ(trades[0].price, price_to_fixed(150));

    // Remaining 60 should still be resting
    EXPECT_EQ(book.askLevels(), 1);
}

TEST_F(OrderBookTest, LimitBuyDoesNotCrossWhenPriceTooLow) {
    book.match(makeLimitSell(155, 100));

    auto cheapBuy = Order::create("acc1", "AAPL", OrderSide::BUY,
                                  OrderType::LIMIT, price_to_fixed(150),
                                  0, 50);
    auto trades = book.match(cheapBuy);

    EXPECT_EQ(trades.size(), 0); // no match — buy at 150 < ask at 155
    EXPECT_EQ(book.bidLevels(), 1);
    EXPECT_EQ(book.askLevels(), 1);
}

// ---- IOC (Immediate-or-Cancel) ----

TEST_F(OrderBookTest, IocFillsPartialAndCancelsRest) {
    book.match(makeLimitSell(150, 30));

    auto iocOrder = Order::create("acc1", "AAPL", OrderSide::BUY,
                                  OrderType::IOC, price_to_fixed(150),
                                  0, 100);
    auto trades = book.match(iocOrder);

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 30);

    // Nothing should rest in the book — remainder cancelled
    EXPECT_EQ(book.bidLevels(), 0);
}

// ---- FOK (Fill-or-Kill) ----

TEST_F(OrderBookTest, FokRejectsWhenCannotFillCompletely) {
    book.match(makeLimitSell(150, 30));

    auto fokOrder = Order::create("acc1", "AAPL", OrderSide::BUY,
                                  OrderType::FOK, price_to_fixed(150),
                                  0, 100);
    auto trades = book.match(fokOrder);

    EXPECT_EQ(trades.size(), 0); // cannot fill 100, only 30 available
    EXPECT_EQ(book.askLevels(), 1); // sell order should still be there
}

TEST_F(OrderBookTest, FokFillsWhenFullQuantityAvailable) {
    book.match(makeLimitSell(150, 100));

    auto fokOrder = Order::create("acc1", "AAPL", OrderSide::BUY,
                                  OrderType::FOK, price_to_fixed(150),
                                  0, 50);
    auto trades = book.match(fokOrder);

    EXPECT_EQ(trades.size(), 1);
    EXPECT_EQ(trades[0].quantity, 50);
}

// ---- Cancel ----

TEST_F(OrderBookTest, CancelRemovesOrderFromBook) {
    auto order = makeLimitBuy(150, 100);
    book.match(order);
    EXPECT_EQ(book.totalOrders(), 1);

    bool cancelled = book.removeFromBook(order.orderId);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(book.totalOrders(), 0);
    EXPECT_EQ(book.bidLevels(), 0);
}

TEST_F(OrderBookTest, CancelNonexistentReturnsFalse) {
    EXPECT_FALSE(book.removeFromBook("nonexistent-id"));
}

// ---- Snapshot ----

TEST_F(OrderBookTest, SnapshotReturnsCorrectDepth) {
    for (int i = 0; i < 5; ++i) {
        book.match(Order::create("acc1", "AAPL", OrderSide::BUY,
                                 OrderType::LIMIT,
                                 price_to_fixed(149 - i), 0, 100));
        book.match(Order::create("acc2", "AAPL", OrderSide::SELL,
                                 OrderType::LIMIT,
                                 price_to_fixed(151 + i), 0, 100));
    }

    auto snap = book.snapshot(3);
    EXPECT_EQ(snap.bids.size(), 3);
    EXPECT_EQ(snap.asks.size(), 3);
    EXPECT_EQ(snap.symbol, "AAPL");
}

// ---- Spread ----

TEST_F(OrderBookTest, SpreadCalculation) {
    book.match(makeLimitBuy(149, 100));
    book.match(makeLimitSell(151, 100));

    auto spread = book.spread();
    ASSERT_TRUE(spread.has_value());
    EXPECT_EQ(*spread, price_to_fixed(2)); // $151 - $149 = $2
}
