#include "titanx/persistence/RedisPositionCache.hpp"
#include <hiredis/hiredis.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace titanx {

RedisPositionCache::RedisPositionCache(const std::string& host, int port)
    : ctx_(nullptr) {

    struct timeval timeout = {1, 500000}; // 1.5 seconds
    ctx_ = redisConnectWithTimeout(host.c_str(), port, timeout);

    if (ctx_ == nullptr || ctx_->err) {
        std::string err = ctx_ ? ctx_->errstr : "allocation failed";
        spdlog::error("RedisPositionCache: connection failed: {}", err);
        if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
        // Don't throw — allow graceful degradation
        return;
    }
    spdlog::info("RedisPositionCache connected to {}:{}", host, port);
}

RedisPositionCache::~RedisPositionCache() {
    close();
}

// ---- Position tracking ----

void RedisPositionCache::updatePosition(const std::string& accountId,
                                        const std::string& symbol,
                                        int64_t delta) {
    if (!ctx_) return;
    std::string key = positionKey(accountId, symbol);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "INCRBY %s %lld", key.c_str(), (long long)delta));
    if (reply) freeReplyObject(reply);
}

int64_t RedisPositionCache::getPosition(const std::string& accountId,
                                        const std::string& symbol) {
    if (!ctx_) return 0;
    std::string key = positionKey(accountId, symbol);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "GET %s", key.c_str()));

    int64_t result = 0;
    if (reply) {
        if (reply->type == REDIS_REPLY_STRING && reply->str) {
            result = std::stoll(reply->str);
        }
        freeReplyObject(reply);
    }
    return result;
}

// ---- Order status cache ----

void RedisPositionCache::setOrderStatus(const std::string& orderId,
                                        const std::string& status) {
    if (!ctx_) return;
    std::string key = orderKey(orderId);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SETEX %s 3600 %s", key.c_str(), status.c_str()));
    if (reply) freeReplyObject(reply);
}

std::string RedisPositionCache::getOrderStatus(const std::string& orderId) {
    if (!ctx_) return "";
    std::string key = orderKey(orderId);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "GET %s", key.c_str()));

    std::string result;
    if (reply) {
        if (reply->type == REDIS_REPLY_STRING && reply->str) {
            result = reply->str;
        }
        freeReplyObject(reply);
    }
    return result;
}

// ---- Pub/sub fill broadcast ----

void RedisPositionCache::publishFill(const std::string& symbol,
                                     const std::string& tradeJson) {
    if (!ctx_) return;
    std::string channel = "fills:" + symbol;
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "PUBLISH %s %s", channel.c_str(), tradeJson.c_str()));
    if (reply) {
        spdlog::debug("Published fill to fills:{}, {} subscribers",
                      symbol, reply->integer);
        freeReplyObject(reply);
    }
}

void RedisPositionCache::close() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

// ---- Key builders ----

std::string RedisPositionCache::positionKey(const std::string& accountId,
                                            const std::string& symbol) const {
    return "pos:" + accountId + ":" + symbol;
}

std::string RedisPositionCache::orderKey(const std::string& orderId) const {
    return "order:" + orderId + ":status";
}

} // namespace titanx
