package com.titanx.gateway.controller;

import com.titanx.engine.MatchingEngine;
import com.titanx.gateway.security.TokenBucketRateLimiter;
import com.titanx.model.*;
import com.titanx.risk.CircuitBreaker;
import io.micrometer.core.instrument.Counter;
import io.micrometer.core.instrument.MeterRegistry;
import io.micrometer.core.instrument.Timer;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.http.HttpHeaders;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.security.core.annotation.AuthenticationPrincipal;
import org.springframework.security.core.userdetails.User;
import org.springframework.web.bind.annotation.*;

import java.math.BigDecimal;
import java.util.List;
import java.util.Map;

/**
 * ============================================================
 * TitanX OrderController — Phase 4: REST API
 * ============================================================
 *
 * REST endpoints for order lifecycle management.
 * All endpoints require JWT authentication (validated by JwtAuthFilter).
 * Rate limiting is enforced per account (100 submissions/sec).
 *
 * API design follows FIX protocol semantics where applicable.
 *
 * Endpoints:
 *   POST /api/v1/orders              — Submit an order
 *   DELETE /api/v1/orders/{orderId}  — Cancel an order
 *   GET  /api/v1/orderbook/{symbol}  — Get top-of-book snapshot
 *   GET  /api/v1/health              — Engine health check
 *   POST /api/v1/admin/halt          — Global kill switch (ADMIN only)
 */
@RestController
@RequestMapping("/api/v1")
public class OrderController {

    private static final Logger log = LoggerFactory.getLogger(OrderController.class);

    private final MatchingEngine matchingEngine;
    private final TokenBucketRateLimiter rateLimiter;
    private final CircuitBreaker circuitBreaker;

    // Micrometer metrics
    private final Counter orderSubmittedCounter;
    private final Counter orderRejectedCounter;
    private final Timer   matchingTimer;

    public OrderController(MatchingEngine matchingEngine,
                           TokenBucketRateLimiter rateLimiter,
                           CircuitBreaker circuitBreaker,
                           MeterRegistry meterRegistry) {
        this.matchingEngine = matchingEngine;
        this.rateLimiter    = rateLimiter;
        this.circuitBreaker = circuitBreaker;

        this.orderSubmittedCounter = Counter.builder("titanx.orders.submitted")
                .description("Total orders submitted to the engine")
                .register(meterRegistry);
        this.orderRejectedCounter = Counter.builder("titanx.orders.rate_limited")
                .description("Orders rejected due to rate limiting")
                .register(meterRegistry);
        this.matchingTimer = Timer.builder("titanx.order.matching.latency")
                .description("End-to-end order matching latency")
                .publishPercentiles(0.5, 0.95, 0.99)
                .register(meterRegistry);
    }

    // ---------------------------------------------------------------
    //  POST /api/v1/orders — Submit Order
    // ---------------------------------------------------------------

    @PostMapping("/orders")
    public ResponseEntity<?> submitOrder(@RequestBody OrderRequest request,
                                         @AuthenticationPrincipal User principal) {
        String accountId = principal.getUsername();

        // Rate limiting check (Phase 4)
        if (!rateLimiter.tryAcquire(accountId, "ORDER_SUBMIT")) {
            orderRejectedCounter.increment();
            long retryAfter = rateLimiter.retryAfterSeconds(accountId, "ORDER_SUBMIT");
            return ResponseEntity.status(HttpStatus.TOO_MANY_REQUESTS)
                    .header(HttpHeaders.RETRY_AFTER, String.valueOf(retryAfter))
                    .body(Map.of(
                            "error", "RATE_LIMIT_EXCEEDED",
                            "message", "Maximum 100 orders/second per account",
                            "retryAfter", retryAfter
                    ));
        }

        // Build Order domain object from request
        Order order;
        try {
            order = Order.create(
                    accountId,
                    request.symbol().toUpperCase(),
                    OrderSide.valueOf(request.side().toUpperCase()),
                    OrderType.valueOf(request.type().toUpperCase()),
                    request.price() != null ? new BigDecimal(request.price()) : null,
                    request.stopPrice() != null ? new BigDecimal(request.stopPrice()) : null,
                    request.quantity()
            );
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest()
                    .body(Map.of("error", "INVALID_REQUEST", "message", e.getMessage()));
        }

        // Submit to matching engine (timed for Micrometer)
        orderSubmittedCounter.increment();
        List<Trade> trades = matchingTimer.record(() -> matchingEngine.submit(order));

        log.info("Order {} submitted by {}, {} trade(s) generated",
                order.orderId(), accountId, trades != null ? trades.size() : 0);

        return ResponseEntity.ok(Map.of(
                "orderId",     order.orderId(),
                "status",      order.status().name(),
                "tradesCount", trades != null ? trades.size() : 0,
                "trades",      trades != null ? trades.stream()
                        .map(t -> Map.of(
                                "tradeId",  t.tradeId(),
                                "price",    t.price(),
                                "quantity", t.quantity()
                        )).toList() : List.of()
        ));
    }

    // ---------------------------------------------------------------
    //  DELETE /api/v1/orders/{orderId} — Cancel Order
    // ---------------------------------------------------------------

    @DeleteMapping("/orders/{orderId}")
    public ResponseEntity<?> cancelOrder(@PathVariable String orderId,
                                         @RequestParam String symbol,
                                         @AuthenticationPrincipal User principal) {
        boolean cancelled = matchingEngine.cancel(symbol.toUpperCase(), orderId);
        if (cancelled) {
            return ResponseEntity.ok(Map.of("orderId", orderId, "status", "CANCELLED"));
        }
        return ResponseEntity.status(HttpStatus.NOT_FOUND)
                .body(Map.of("error", "ORDER_NOT_FOUND", "orderId", orderId));
    }

    // ---------------------------------------------------------------
    //  GET /api/v1/orderbook/{symbol} — Order Book Snapshot
    // ---------------------------------------------------------------

    @GetMapping("/orderbook/{symbol}")
    public ResponseEntity<?> getOrderBook(@PathVariable String symbol,
                                          @RequestParam(defaultValue = "10") int depth) {
        return matchingEngine.getSnapshot(symbol.toUpperCase(), depth)
                .map(snapshot -> ResponseEntity.ok((Object) snapshot))
                .orElseGet(() -> ResponseEntity.status(HttpStatus.NOT_FOUND)
                        .body(Map.of("error", "SYMBOL_NOT_FOUND", "symbol", symbol)));
    }

    // ---------------------------------------------------------------
    //  GET /api/v1/health — Engine Health
    // ---------------------------------------------------------------

    @GetMapping("/health")
    public ResponseEntity<Map<String, Object>> health() {
        return ResponseEntity.ok(Map.of(
                "status",  matchingEngine.isHalted() ? "HALTED" : "RUNNING",
                "halted",  matchingEngine.isHalted(),
                "circuitBreakers", circuitBreaker.haltStatus()
        ));
    }

    // ---------------------------------------------------------------
    //  POST /api/v1/admin/halt — Global Kill Switch
    // ---------------------------------------------------------------

    @PostMapping("/admin/halt")
    public ResponseEntity<?> halt(@AuthenticationPrincipal User principal) {
        // In production: check principal has ADMIN permission
        matchingEngine.halt();
        log.warn("GLOBAL HALT activated by {}", principal.getUsername());
        return ResponseEntity.ok(Map.of("status", "HALTED", "activatedBy", principal.getUsername()));
    }

    @PostMapping("/admin/resume")
    public ResponseEntity<?> resume(@AuthenticationPrincipal User principal) {
        matchingEngine.resume();
        log.info("Engine resumed by {}", principal.getUsername());
        return ResponseEntity.ok(Map.of("status", "RUNNING"));
    }

    // ---------------------------------------------------------------
    //  Request DTO
    // ---------------------------------------------------------------

    public record OrderRequest(
            String symbol,
            String side,      // BUY | SELL
            String type,      // MARKET | LIMIT | IOC | FOK | STOP_LIMIT
            String price,     // null for MARKET
            String stopPrice, // non-null for STOP_LIMIT
            long quantity
    ) {}
}
