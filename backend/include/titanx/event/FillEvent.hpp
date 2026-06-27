#pragma once

#include "titanx/model/Trade.hpp"
#include <chrono>

namespace titanx {

/**
 * Fill notification published to Redis pub/sub.
 * Consumed by WebSocket handlers for real-time updates.
 */
struct FillEvent {
    Trade trade;
    long  matchingLatencyMicros;
    SysTimePoint publishedAt;

    FillEvent(const Trade& t, long latencyMicros)
        : trade(t)
        , matchingLatencyMicros(latencyMicros)
        , publishedAt(SystemClock::now()) {}

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["trade"]                 = trade.to_json();
        j["matchingLatencyMicros"] = matchingLatencyMicros;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            publishedAt.time_since_epoch()).count();
        j["publishedAt"] = ms;
        return j;
    }
};

} // namespace titanx
