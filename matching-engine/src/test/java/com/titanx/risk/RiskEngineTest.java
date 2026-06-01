package com.titanx.risk;

import com.titanx.model.*;
import org.junit.jupiter.api.*;

import java.math.BigDecimal;

import static org.assertj.core.api.Assertions.*;

@DisplayName("TitanX RiskEngine — Pre-Trade Check Tests")
class RiskEngineTest {

    private RiskEngine riskEngine;
    private CircuitBreaker circuitBreaker;

    @BeforeEach
    void setUp() {
        circuitBreaker = new CircuitBreaker();
        riskEngine = new RiskEngine(circuitBreaker);
    }

    @Test
    @DisplayName("Valid order passes all risk checks")
    void validOrderIsApproved() {
        Order order = Order.create("acc-001", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("150.00"), null, 100);
        RiskResult result = riskEngine.checkPreTrade(order);
        assertThat(result.approved()).isTrue();
    }

    @Test
    @DisplayName("Order exceeding notional limit is rejected")
    void notionalLimitRejected() {
        // price * qty = 600.00 * 10000 = $6M > $5M limit
        Order bigOrder = Order.create("acc-001", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("600.00"), null, 10_000);
        RiskResult result = riskEngine.checkPreTrade(bigOrder);
        assertThat(result.approved()).isFalse();
        assertThat(result.reason()).isEqualTo("NOTIONAL_LIMIT_EXCEEDED");
    }

    @Test
    @DisplayName("Duplicate order ID is rejected")
    void duplicateOrderIdRejected() {
        Order order = Order.create("acc-001", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 50);
        riskEngine.checkPreTrade(order);  // first submission — accepted

        // Simulate replay with same order object (same orderId)
        RiskResult duplicate = riskEngine.checkPreTrade(order);
        assertThat(duplicate.approved()).isFalse();
        assertThat(duplicate.reason()).isEqualTo("DUPLICATE_ORDER_ID");
    }

    @Test
    @DisplayName("Order rejected when circuit breaker is triggered")
    void circuitBreakerRejectsOrder() {
        circuitBreaker.haltSymbol("AAPL", "Test halt");

        Order order = Order.create("acc-001", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 100);
        RiskResult result = riskEngine.checkPreTrade(order);

        assertThat(result.approved()).isFalse();
        assertThat(result.reason()).isEqualTo("CIRCUIT_BREAKER_HALT");
    }

    @Test
    @DisplayName("Position limit exceeded after accumulated fills")
    void positionLimitExceeded() {
        // Build up a position via recordFill
        riskEngine.recordFill("acc-001", "AAPL", OrderSide.BUY, 9_500);

        // Now try to buy 1000 more — would take position to 10,500 > 10,000 limit
        Order order = Order.create("acc-001", "AAPL", OrderSide.BUY, OrderType.LIMIT,
                new BigDecimal("100.00"), null, 1_000);
        RiskResult result = riskEngine.checkPreTrade(order);

        assertThat(result.approved()).isFalse();
        assertThat(result.reason()).isEqualTo("POSITION_LIMIT_EXCEEDED");
    }

    @Test
    @DisplayName("Circuit breaker triggers on large price move")
    void circuitBreakerTriggersOnPriceMove() {
        circuitBreaker.recordTrade("TSLA", new BigDecimal("200.00")); // reference price
        // Simulate a 15% crash — exceeds 5% threshold
        circuitBreaker.recordTrade("TSLA", new BigDecimal("169.00"));

        assertThat(circuitBreaker.isHalted("TSLA")).isTrue();
    }

    @Test
    @DisplayName("Circuit breaker resumes after manual clear")
    void circuitBreakerResumes() {
        circuitBreaker.haltSymbol("NVDA", "Test");
        assertThat(circuitBreaker.isHalted("NVDA")).isTrue();

        circuitBreaker.resumeSymbol("NVDA");
        assertThat(circuitBreaker.isHalted("NVDA")).isFalse();
    }
}
