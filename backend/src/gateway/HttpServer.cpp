#include "titanx/gateway/HttpServer.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>

namespace titanx {

HttpServer::HttpServer(MatchingEngine& engine,
                       CircuitBreaker& circuitBreaker,
                       JwtTokenService& jwtService,
                       TokenBucketRateLimiter& rateLimiter,
                       uint16_t port)
    : engine_(engine)
    , circuitBreaker_(circuitBreaker)
    , jwtService_(jwtService)
    , rateLimiter_(rateLimiter)
    , port_(port) {

    setupRoutes();
}

// ---------------------------------------------------------------
//  JWT Authentication Helper
// ---------------------------------------------------------------

std::pair<bool, JwtClaims> HttpServer::authenticate(const crow::request& req) {
    std::string authHeader = req.get_header_value("Authorization");
    if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
        return {false, JwtClaims{.valid = false, .error = "Missing Authorization header"}};
    }

    std::string token = authHeader.substr(7);
    JwtClaims claims = jwtService_.validateAndExtract(token);
    return {claims.valid, claims};
}

// ---------------------------------------------------------------
//  Route Setup
// ---------------------------------------------------------------

void HttpServer::setupRoutes() {

    // ---- POST /api/v1/auth/login ----
    CROW_ROUTE(app_, "/api/v1/auth/login").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        try {
            auto body = nlohmann::json::parse(req.body);
            std::string accountId = body.value("accountId", "");
            std::string password  = body.value("password", "");

            if (accountId.empty()) {
                return crow::response(400,
                    nlohmann::json({{"error", "INVALID_REQUEST"},
                                    {"message", "accountId required"}}).dump());
            }

            // Simplified auth — in production, check against account DB
            if (password != "titan") {
                return crow::response(401,
                    nlohmann::json({{"error", "UNAUTHORIZED"},
                                    {"message", "Invalid credentials"}}).dump());
            }

            std::string accessToken = jwtService_.generateAccessToken(
                accountId, {"READ", "TRADE"});
            std::string refreshToken = jwtService_.generateRefreshToken(
                accountId, generate_uuid());

            nlohmann::json resp;
            resp["accessToken"]  = accessToken;
            resp["refreshToken"] = refreshToken;
            resp["expiresIn"]    = 900; // 15 minutes in seconds

            auto r = crow::response(200, resp.dump());
            r.set_header("Content-Type", "application/json");
            return r;
        } catch (const std::exception& e) {
            return crow::response(400,
                nlohmann::json({{"error", "INVALID_REQUEST"},
                                {"message", e.what()}}).dump());
        }
    });

    // ---- POST /api/v1/orders ----
    CROW_ROUTE(app_, "/api/v1/orders").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        // Authenticate
        auto [authed, claims] = authenticate(req);
        if (!authed) {
            return crow::response(401,
                nlohmann::json({{"error", "UNAUTHORIZED"},
                                {"message", claims.error}}).dump());
        }

        // Rate limiting
        if (!rateLimiter_.tryAcquire(claims.accountId, "ORDER_SUBMIT")) {
            long retryAfter = rateLimiter_.retryAfterSeconds(
                claims.accountId, "ORDER_SUBMIT");
            auto r = crow::response(429,
                nlohmann::json({{"error", "RATE_LIMIT_EXCEEDED"},
                    {"message", "Maximum 100 orders/second per account"},
                    {"retryAfter", retryAfter}}).dump());
            r.set_header("Retry-After", std::to_string(retryAfter));
            r.set_header("Content-Type", "application/json");
            return r;
        }

        try {
            auto body = nlohmann::json::parse(req.body);

            std::string symbolStr = body.value("symbol", "");
            std::string sideStr   = body.value("side", "");
            std::string typeStr   = body.value("type", "");
            int64_t quantity      = body.value("quantity", 0LL);

            // Convert symbol to uppercase
            std::transform(symbolStr.begin(), symbolStr.end(),
                           symbolStr.begin(), ::toupper);
            std::transform(sideStr.begin(), sideStr.end(),
                           sideStr.begin(), ::toupper);
            std::transform(typeStr.begin(), typeStr.end(),
                           typeStr.begin(), ::toupper);

            int64_t price = 0;
            if (body.contains("price") && !body["price"].is_null()) {
                if (body["price"].is_string()) {
                    price = price_to_fixed(body["price"].get<std::string>());
                } else {
                    price = price_to_fixed(body["price"].get<double>());
                }
            }

            int64_t stopPrice = 0;
            if (body.contains("stopPrice") && !body["stopPrice"].is_null()) {
                if (body["stopPrice"].is_string()) {
                    stopPrice = price_to_fixed(body["stopPrice"].get<std::string>());
                } else {
                    stopPrice = price_to_fixed(body["stopPrice"].get<double>());
                }
            }

            Order order = Order::create(
                claims.accountId, symbolStr,
                order_side_from_string(sideStr),
                order_type_from_string(typeStr),
                price, stopPrice, quantity);

            auto trades = engine_.submit(order);

            // Build response
            nlohmann::json resp;
            resp["orderId"]     = order.orderId;
            resp["status"]      = titanx::to_string(order.status);
            resp["tradesCount"] = trades.size();
            resp["trades"]      = nlohmann::json::array();
            for (const auto& t : trades) {
                resp["trades"].push_back({
                    {"tradeId", t.tradeId},
                    {"price", price_to_string(t.price)},
                    {"quantity", t.quantity}
                });
            }

            spdlog::info("Order {} submitted by {}, {} trade(s)",
                         order.orderId, claims.accountId, trades.size());

            auto r = crow::response(200, resp.dump());
            r.set_header("Content-Type", "application/json");
            return r;

        } catch (const std::exception& e) {
            return crow::response(400,
                nlohmann::json({{"error", "INVALID_REQUEST"},
                                {"message", e.what()}}).dump());
        }
    });

    // ---- DELETE /api/v1/orders/:orderId ----
    CROW_ROUTE(app_, "/api/v1/orders/<string>").methods(crow::HTTPMethod::DELETE)
    ([this](const crow::request& req, const std::string& orderId) {
        auto [authed, claims] = authenticate(req);
        if (!authed) {
            return crow::response(401,
                nlohmann::json({{"error", "UNAUTHORIZED"}}).dump());
        }

        // Get symbol from query param
        auto symbolParam = req.url_params.get("symbol");
        if (!symbolParam) {
            return crow::response(400,
                nlohmann::json({{"error", "MISSING_PARAM"},
                                {"message", "symbol query parameter required"}}).dump());
        }

        std::string symbol = symbolParam;
        std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);

        bool cancelled = engine_.cancel(symbol, orderId);
        if (cancelled) {
            auto r = crow::response(200,
                nlohmann::json({{"orderId", orderId},
                                {"status", "CANCELLED"}}).dump());
            r.set_header("Content-Type", "application/json");
            return r;
        }

        return crow::response(404,
            nlohmann::json({{"error", "ORDER_NOT_FOUND"},
                            {"orderId", orderId}}).dump());
    });

    // ---- GET /api/v1/orderbook/:symbol ----
    CROW_ROUTE(app_, "/api/v1/orderbook/<string>").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req, const std::string& symbol) {
        int depth = 10;
        auto depthParam = req.url_params.get("depth");
        if (depthParam) {
            depth = std::atoi(depthParam);
            if (depth <= 0) depth = 10;
        }

        std::string upperSymbol = symbol;
        std::transform(upperSymbol.begin(), upperSymbol.end(),
                       upperSymbol.begin(), ::toupper);

        auto snapshot = engine_.getSnapshot(upperSymbol, depth);
        if (snapshot) {
            auto r = crow::response(200, snapshot->to_json().dump());
            r.set_header("Content-Type", "application/json");
            return r;
        }

        return crow::response(404,
            nlohmann::json({{"error", "SYMBOL_NOT_FOUND"},
                            {"symbol", symbol}}).dump());
    });

    // ---- GET /api/v1/health ----
    CROW_ROUTE(app_, "/api/v1/health").methods(crow::HTTPMethod::GET)
    ([this](const crow::request&) {
        nlohmann::json resp;
        resp["status"]  = engine_.isHalted() ? "HALTED" : "RUNNING";
        resp["halted"]  = engine_.isHalted();

        auto cbStatus = circuitBreaker_.haltStatus();
        nlohmann::json cbJson;
        for (const auto& [sym, halted] : cbStatus) {
            cbJson[sym] = halted;
        }
        resp["circuitBreakers"] = cbJson;

        auto r = crow::response(200, resp.dump());
        r.set_header("Content-Type", "application/json");
        return r;
    });

    // ---- POST /api/v1/admin/halt ----
    CROW_ROUTE(app_, "/api/v1/admin/halt").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        auto [authed, claims] = authenticate(req);
        if (!authed) {
            return crow::response(401,
                nlohmann::json({{"error", "UNAUTHORIZED"}}).dump());
        }

        engine_.halt();
        spdlog::warn("GLOBAL HALT activated by {}", claims.accountId);

        auto r = crow::response(200,
            nlohmann::json({{"status", "HALTED"},
                            {"activatedBy", claims.accountId}}).dump());
        r.set_header("Content-Type", "application/json");
        return r;
    });

    // ---- POST /api/v1/admin/resume ----
    CROW_ROUTE(app_, "/api/v1/admin/resume").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        auto [authed, claims] = authenticate(req);
        if (!authed) {
            return crow::response(401,
                nlohmann::json({{"error", "UNAUTHORIZED"}}).dump());
        }

        engine_.resume();
        spdlog::info("Engine resumed by {}", claims.accountId);

        auto r = crow::response(200,
            nlohmann::json({{"status", "RUNNING"}}).dump());
        r.set_header("Content-Type", "application/json");
        return r;
    });
}

void HttpServer::start() {
    spdlog::info("TitanX HTTP server starting on port {}", port_);
    app_.port(port_)
        .multithreaded()
        .run();
}

void HttpServer::stop() {
    app_.stop();
}

} // namespace titanx
