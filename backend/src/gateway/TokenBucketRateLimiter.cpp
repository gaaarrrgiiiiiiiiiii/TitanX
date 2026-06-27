#include "titanx/gateway/TokenBucketRateLimiter.hpp"

namespace titanx {

bool TokenBucketRateLimiter::tryAcquire(const std::string& accountId,
                                         const std::string& endpoint) {
    std::string key = accountId + ":" + endpoint;

    TokenBucket* bucket;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buckets_.find(key);
        if (it == buckets_.end()) {
            buckets_[key] = std::make_unique<TokenBucket>(
                STANDARD_CAPACITY, STANDARD_REFILL_RATE);
            it = buckets_.find(key);
        }
        bucket = it->second.get();
    }

    return bucket->tryConsume();
}

long TokenBucketRateLimiter::retryAfterSeconds(const std::string& accountId,
                                                const std::string& endpoint) {
    std::string key = accountId + ":" + endpoint;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buckets_.find(key);
    if (it == buckets_.end()) return 0;
    return it->second->secondsUntilRefill();
}

// ---------------------------------------------------------------
//  TokenBucket implementation
// ---------------------------------------------------------------

bool TokenBucketRateLimiter::TokenBucket::tryConsume() {
    refill();

    int64_t current = tokens.load(std::memory_order_relaxed);
    while (current > 0) {
        if (tokens.compare_exchange_weak(current, current - 1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
        // current is updated by compare_exchange_weak on failure
    }
    return false;
}

long TokenBucketRateLimiter::TokenBucket::secondsUntilRefill() {
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto lastMs = lastRefillTimeMs.load(std::memory_order_relaxed);
    auto elapsed = nowMs - lastMs;
    int64_t tokensToAdd = (elapsed * refillPerSecond) / 1000;
    if (tokensToAdd > 0) return 0; // refill imminent
    return 1;
}

void TokenBucketRateLimiter::TokenBucket::refill() {
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto lastMs = lastRefillTimeMs.load(std::memory_order_relaxed);
    auto elapsed = nowMs - lastMs;
    if (elapsed <= 0) return;

    int64_t tokensToAdd = (elapsed * refillPerSecond) / 1000;
    if (tokensToAdd > 0) {
        int64_t current = tokens.load(std::memory_order_relaxed);
        int64_t updated = std::min(capacity, current + tokensToAdd);
        tokens.store(updated, std::memory_order_relaxed);
        lastRefillTimeMs.store(nowMs, std::memory_order_relaxed);
    }
}

} // namespace titanx
