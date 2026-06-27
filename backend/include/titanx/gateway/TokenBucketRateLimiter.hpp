#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace titanx {

/**
 * ============================================================
 * TitanX TokenBucketRateLimiter — API Rate Limiting (C++17)
 * ============================================================
 *
 * Algorithm: Token Bucket (per account, per endpoint category)
 *
 * Why token bucket over fixed-window?
 *   Fixed-window counters allow a 2x burst at window boundaries.
 *   Token bucket provides smooth rate with burst capacity.
 *
 * Configuration:
 *   Standard accounts: 100 order submissions/second
 *
 * Lock-free CAS loop for token consumption.
 */
class TokenBucketRateLimiter {
public:
    bool tryAcquire(const std::string& accountId, const std::string& endpoint);
    long retryAfterSeconds(const std::string& accountId,
                           const std::string& endpoint);

private:
    static constexpr int64_t STANDARD_CAPACITY    = 100;
    static constexpr int64_t STANDARD_REFILL_RATE  = 100; // tokens per second

    struct TokenBucket {
        int64_t capacity;
        int64_t refillPerSecond;
        std::atomic<int64_t> tokens;
        std::atomic<int64_t> lastRefillTimeMs;

        TokenBucket(int64_t cap, int64_t refill)
            : capacity(cap)
            , refillPerSecond(refill)
            , tokens(cap)
            , lastRefillTimeMs(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count()) {}

        bool tryConsume();
        long secondsUntilRefill();

    private:
        void refill();
    };

    std::unordered_map<std::string, std::unique_ptr<TokenBucket>> buckets_;
    std::mutex mutex_; // protects map, not the buckets themselves
};

} // namespace titanx
