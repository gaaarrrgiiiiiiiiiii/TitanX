#include "titanx/engine/MatchingEngine.hpp"
#include "titanx/risk/RiskEngine.hpp"
#include "titanx/risk/CircuitBreaker.hpp"
#include "titanx/persistence/EventStore.hpp"
#include "titanx/persistence/RedisPositionCache.hpp"
#include "titanx/gateway/HttpServer.hpp"
#include "titanx/gateway/JwtTokenService.hpp"
#include "titanx/gateway/TokenBucketRateLimiter.hpp"

#include <spdlog/spdlog.h>
#include <csignal>
#include <cstdlib>
#include <string>

/**
 * ============================================================
 * TitanX — Main Application Entry Point
 * ============================================================
 *
 * Wires all components together and starts the HTTP server.
 *
 * Component dependency graph:
 *   CircuitBreaker ← RiskEngine ← MatchingEngine ← HttpServer
 *                                                 ← EventStore
 *                                                 ← RedisPositionCache
 *
 * Configuration via environment variables:
 *   TITANX_DB_URL   — PostgreSQL connection string
 *   TITANX_REDIS_HOST — Redis hostname
 *   TITANX_REDIS_PORT — Redis port
 *   TITANX_HTTP_PORT  — HTTP server port (default: 8081)
 */

static std::atomic<bool> g_running{true};

static void signalHandler(int signum) {
    spdlog::info("Received signal {}, shutting down...", signum);
    g_running.store(false, std::memory_order_relaxed);
}

static std::string getEnv(const std::string& key, const std::string& defaultVal) {
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : defaultVal;
}

int main() {
    // ---- Setup logging ----
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

    spdlog::info("================================================");
    spdlog::info("  TitanX Matching Engine v1.0.0 (C++)");
    spdlog::info("  High-Performance Order Matching System");
    spdlog::info("================================================");

    // ---- Signal handling for graceful shutdown ----
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ---- Configuration ----
    std::string dbUrl     = getEnv("TITANX_DB_URL",
        "host=localhost port=5432 dbname=titanx user=titanx password=titanx");
    std::string redisHost = getEnv("TITANX_REDIS_HOST", "localhost");
    int redisPort         = std::stoi(getEnv("TITANX_REDIS_PORT", "6379"));
    int httpPort          = std::stoi(getEnv("TITANX_HTTP_PORT", "8081"));

    // ---- Wire up components ----
    spdlog::info("Initializing components...");

    // 1. Risk layer
    titanx::CircuitBreaker circuitBreaker;
    titanx::RiskEngine riskEngine(circuitBreaker);

    // 2. Matching engine
    titanx::MatchingEngine engine(riskEngine);

    // 3. Persistence (non-blocking — gracefully degrades if unavailable)
    titanx::EventStore eventStore(dbUrl);
    titanx::RedisPositionCache redisCache(redisHost, redisPort);

    // 4. Wire event consumers
    engine.setEventConsumer([&eventStore](const titanx::OrderEvent& event) {
        eventStore.append(event);
    });

    engine.setFillConsumer([&redisCache, &riskEngine](const titanx::FillEvent& fill) {
        // Update Redis position cache
        titanx::OrderSide side = fill.trade.aggressorSide;
        int64_t delta = (side == titanx::OrderSide::BUY) ? fill.trade.quantity
                                                         : -fill.trade.quantity;
        redisCache.updatePosition(fill.trade.aggressorAccountId,
                                  fill.trade.symbol, delta);
        redisCache.updatePosition(fill.trade.passiveAccountId,
                                  fill.trade.symbol, -delta);

        // Publish fill to Redis pub/sub
        redisCache.publishFill(fill.trade.symbol, fill.trade.to_json().dump());

        // Update risk engine
        riskEngine.recordFill(fill.trade.aggressorAccountId,
                              fill.trade.symbol, fill.trade.aggressorSide,
                              fill.trade.quantity);
        riskEngine.updateLastTradePrice(fill.trade.symbol, fill.trade.price);
    });

    // 5. Gateway layer
    titanx::JwtTokenService jwtService;
    titanx::TokenBucketRateLimiter rateLimiter;
    titanx::HttpServer server(engine, circuitBreaker, jwtService,
                              rateLimiter, static_cast<uint16_t>(httpPort));

    spdlog::info("All components initialized successfully");
    spdlog::info("Starting HTTP server on port {}...", httpPort);

    // ---- Start server (blocking) ----
    server.start();

    // ---- Cleanup ----
    spdlog::info("Shutting down...");
    eventStore.close();
    redisCache.close();
    spdlog::info("TitanX shutdown complete");

    return 0;
}
