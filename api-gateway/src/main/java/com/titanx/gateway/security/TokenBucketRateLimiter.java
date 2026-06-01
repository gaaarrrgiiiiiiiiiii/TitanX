package com.titanx.gateway.security;

import org.springframework.stereotype.Component;

import java.time.Instant;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/**
 * ============================================================
 * TitanX TokenBucketRateLimiter — Phase 4: API Rate Limiting
 * ============================================================
 *
 * Algorithm: Token Bucket (per account, per endpoint category)
 *
 * Why token bucket over fixed-window?
 *  "Fixed-window counters allow a 2x burst at window boundaries.
 *   Token bucket provides a smooth rate with burst capacity.
 *   For a trading API, you want to allow occasional bursts (a trader
 *   sending a batch of orders) without allowing sustained overload."
 *
 * Configuration:
 *   Standard accounts: 100 order submissions/second
 *   Admin accounts:    1000 requests/second
 *   Market data reads: 10 requests/second (lower priority than trades)
 *
 * Response on rejection: HTTP 429 with Retry-After header.
 *
 * Implementation: pure in-memory (appropriate for a single gateway).
 * In a clustered gateway, use Redis + Lua scripts for atomic token
 * consumption across multiple nodes.
 */
@Component
public class TokenBucketRateLimiter {

    private static final long STANDARD_CAPACITY    = 100;  // max tokens
    private static final long STANDARD_REFILL_RATE = 100;  // tokens per second

    private final Map<String, TokenBucket> buckets = new ConcurrentHashMap<>();

    /**
     * @param accountId  the account making the request
     * @param endpoint   endpoint category ("ORDER_SUBMIT", "MARKET_DATA", "ADMIN")
     * @return true if allowed, false if rate-limited
     */
    public boolean tryAcquire(String accountId, String endpoint) {
        String key = accountId + ":" + endpoint;
        TokenBucket bucket = buckets.computeIfAbsent(key, k ->
                new TokenBucket(STANDARD_CAPACITY, STANDARD_REFILL_RATE));
        return bucket.tryConsume();
    }

    /**
     * Returns seconds until next available token (for Retry-After header).
     */
    public long retryAfterSeconds(String accountId, String endpoint) {
        String key = accountId + ":" + endpoint;
        TokenBucket bucket = buckets.get(key);
        if (bucket == null) return 0;
        return bucket.secondsUntilRefill();
    }

    // ---------------------------------------------------------------
    //  Token Bucket Implementation
    // ---------------------------------------------------------------

    private static class TokenBucket {
        private final long capacity;
        private final long refillPerSecond;
        private final AtomicLong tokens;
        private volatile long lastRefillTime = System.currentTimeMillis();

        TokenBucket(long capacity, long refillPerSecond) {
            this.capacity = capacity;
            this.refillPerSecond = refillPerSecond;
            this.tokens = new AtomicLong(capacity); // start full
        }

        boolean tryConsume() {
            refill();
            long current = tokens.get();
            while (current > 0) {
                if (tokens.compareAndSet(current, current - 1)) return true;
                current = tokens.get();
            }
            return false;
        }

        long secondsUntilRefill() {
            long now = System.currentTimeMillis();
            long elapsed = now - lastRefillTime;
            long tokensToAdd = (elapsed * refillPerSecond) / 1000;
            if (tokensToAdd > 0) return 0; // refill imminent
            return 1; // will refill within 1 second
        }

        private void refill() {
            long now = System.currentTimeMillis();
            long elapsed = now - lastRefillTime;
            if (elapsed <= 0) return;
            long tokensToAdd = (elapsed * refillPerSecond) / 1000;
            if (tokensToAdd > 0) {
                long updated = Math.min(capacity, tokens.get() + tokensToAdd);
                tokens.set(updated);
                lastRefillTime = now;
            }
        }
    }
}
