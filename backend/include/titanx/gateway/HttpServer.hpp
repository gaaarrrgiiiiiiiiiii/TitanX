#pragma once

#include "titanx/engine/MatchingEngine.hpp"
#include "titanx/gateway/JwtTokenService.hpp"
#include "titanx/gateway/TokenBucketRateLimiter.hpp"
#include "titanx/risk/CircuitBreaker.hpp"

#include <crow.h>
#include <string>
#include <cstdint>

namespace titanx {

/**
 * ============================================================
 * TitanX HttpServer — REST API Gateway (C++17 / Crow)
 * ============================================================
 *
 * Replaces Spring Boot 3 with Crow — a lightweight C++ HTTP framework.
 * Same REST endpoints, same JSON schemas, same JWT authentication.
 *
 * Endpoints:
 *   POST /api/v1/orders              — Submit an order
 *   DELETE /api/v1/orders/:orderId   — Cancel an order
 *   GET  /api/v1/orderbook/:symbol   — Order book snapshot
 *   GET  /api/v1/health              — Engine health check
 *   POST /api/v1/auth/login          — Get JWT token
 *   POST /api/v1/admin/halt          — Global kill switch
 *   POST /api/v1/admin/resume        — Resume trading
 */
class HttpServer {
public:
    HttpServer(MatchingEngine& engine,
               CircuitBreaker& circuitBreaker,
               JwtTokenService& jwtService,
               TokenBucketRateLimiter& rateLimiter,
               uint16_t port = 8081);

    void start();
    void stop();

private:
    MatchingEngine&         engine_;
    CircuitBreaker&         circuitBreaker_;
    JwtTokenService&        jwtService_;
    TokenBucketRateLimiter& rateLimiter_;
    uint16_t                port_;

    crow::SimpleApp app_;

    void setupRoutes();

    // JWT middleware helper
    std::pair<bool, JwtClaims> authenticate(const crow::request& req);
};

} // namespace titanx
