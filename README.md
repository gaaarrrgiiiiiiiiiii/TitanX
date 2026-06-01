# TitanX — High-Performance Order Matching Engine

> *Targeting Goldman Sachs Strats / Engineering · Built with the same design constraints that real exchanges use.*

[![Java 21](https://img.shields.io/badge/Java-21-orange?logo=java)](https://openjdk.org/projects/jdk/21/)
[![Spring Boot](https://img.shields.io/badge/Spring%20Boot-3.2-green?logo=springboot)](https://spring.io/projects/spring-boot)
[![Redis](https://img.shields.io/badge/Redis-7.2-red?logo=redis)](https://redis.io/)
[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-16-blue?logo=postgresql)](https://www.postgresql.org/)
[![Prometheus](https://img.shields.io/badge/Prometheus-Grafana-orange?logo=grafana)](https://grafana.com/)

---

## The Problem

Most student "trading projects" are CRUD apps in disguise: an orders table in SQL, a loop that checks for matches, and a REST API on top. **TitanX is different.**

The core challenge is **latency and correctness under concurrency** — the same category of problem that Jane Street, Citadel, and Goldman's Strats team solve daily. The engineering question isn't *"can I store an order?"* — it's:

> *"Can I match 100,000 orders per minute with zero incorrect fills, zero lost events, and no global lock — while running circuit breakers, risk checks, and broadcasting fills to WebSocket subscribers simultaneously?"*

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         Client / FIX Gateway                    │
└──────────────────────────┬──────────────────────────────────────┘
                           │ HTTPS / WebSocket (JWT-secured)
┌──────────────────────────▼──────────────────────────────────────┐
│                    API Gateway (Spring Boot 3)                   │
│  ┌────────────────────┐  ┌──────────────────────────────────┐   │
│  │ JwtAuthFilter      │  │ TokenBucketRateLimiter            │   │
│  │ 15-min access tkn  │  │ 100 orders/sec per account        │   │
│  └────────────────────┘  └──────────────────────────────────┘   │
│  ┌────────────────────┐  ┌──────────────────────────────────┐   │
│  │ OrderController    │  │ WebSocketHandler (fills stream)   │   │
│  │ REST CRUD          │  │ Redis pub/sub → WS push           │   │
│  └────────────────────┘  └──────────────────────────────────┘   │
└──────────────────────────┬──────────────────────────────────────┘
                           │ in-process call (same JVM)
┌──────────────────────────▼──────────────────────────────────────┐
│               Matching Engine (Java 21)                          │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  RiskEngine (pre-trade checks run BEFORE book entry)     │    │
│  │  · Position limits (10K shares net long/short)           │    │
│  │  · Notional limits ($5M per order)                       │    │
│  │  · Self-trade prevention (RegNMS)                        │    │
│  │  · Fat-finger: 10% price deviation                       │    │
│  │  · Duplicate order ID (idempotency)                      │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  MatchingEngine (per-symbol ReentrantReadWriteLock)      │    │
│  │  · Different symbols match concurrently                  │    │
│  │  · Single write lock per symbol for correctness          │    │
│  │  · Read lock for concurrent market data queries          │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  OrderBook (per symbol)                                  │    │
│  │  bids: TreeMap<price↓, PriorityQueue<Order>>            │    │
│  │  asks: TreeMap<price↑, PriorityQueue<Order>>            │    │
│  │  Price-time priority · MARKET · LIMIT · IOC · FOK       │    │
│  │  Stop-limit trigger on last trade price                  │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  CircuitBreaker                                          │    │
│  │  · Per-symbol halt if price moves >5% in 60s            │    │
│  │  · Global kill switch (volatile boolean, no lock)        │    │
│  └─────────────────────────────────────────────────────────┘    │
└──────┬───────────────────────────────────────┬───────────────────┘
       │ async, non-blocking                    │ async pub/sub
┌──────▼──────────┐                    ┌────────▼────────┐
│  PostgreSQL 16  │                    │    Redis 7.2    │
│  order_events   │                    │  positions (hot)│
│  (append-only)  │                    │  fills channel  │
│  Event sourcing │                    │  AOF persistence│
└─────────────────┘                    └─────────────────┘
       │
┌──────▼──────────┐
│  Prometheus +   │
│  Grafana        │
│  p50/p95/p99    │
│  latency heatmap│
└─────────────────┘
```

---

## Key Design Decisions

### 1. OrderBook: `TreeMap<BigDecimal, PriorityQueue<Order>>`

```java
// Bids: descending (best bid = highest price = first entry)
private final TreeMap<BigDecimal, PriorityQueue<Order>> bids =
        new TreeMap<>(Comparator.reverseOrder());

// Asks: ascending (best ask = lowest price = first entry)
private final TreeMap<BigDecimal, PriorityQueue<Order>> asks = new TreeMap<>();
```

**Why `TreeMap` and not `HashMap`?** Matching requires sorted iteration — you must walk price levels in order. `TreeMap` gives `O(log n)` insert/delete and `O(1)` `firstKey()` for best bid/ask. `HashMap` cannot do this.

**Why `BigDecimal` for price?** Floating-point representation errors (`0.1 + 0.2 ≠ 0.3`) are unacceptable in financial systems. `BigDecimal` gives exact decimal arithmetic.

**Within each price level: `PriorityQueue<Order>` sorted by arrival time** — this implements **price-time priority**, the standard rule used by NYSE, NASDAQ, and CME.

---

### 2. Concurrency: Per-Symbol `ReentrantReadWriteLock`

```java
// Different symbols match concurrently (AAPL lock ≠ TSLA lock)
private final ConcurrentHashMap<String, ReentrantReadWriteLock> locks =
        new ConcurrentHashMap<>();
```

- **Write lock** for order submission and cancellation (modifies the book)
- **Read lock** for market data queries (concurrent reads from WebSocket subscribers)

**What's next?** The LMAX Disruptor pattern: a single-threaded matching loop consuming from a lock-free ring buffer. This eliminates lock contention entirely and achieves sub-microsecond latency. LMAX demonstrated 6 million transactions/sec on commodity hardware using this approach.

---

### 3. Event Sourcing: Append-Only PostgreSQL

```sql
-- We never UPDATE or DELETE
INSERT INTO order_events (event_id, event_type, order_id, account_id, symbol, payload, occurred_at)
VALUES (?, ?, ?, ?, ?, ?::jsonb, ?)
```

Every state change generates an immutable `OrderEvent` appended to PostgreSQL. Current state is a **projection** over the event log.

**Why?**
1. **Crash recovery**: Replay events from T=0 to reconstruct book state
2. **Audit trail**: Required by MiFID II and RegNMS
3. **Time-travel queries**: *"What was the order book at 14:32:07.123456?"*

This pattern is how Bloomberg and ICE build their systems.

---

### 4. Risk Engine: Pre-Trade Checks

Checks run **before** an order enters the book — not inside the hot matching loop:

| Check | Limit | Rejection Code |
|---|---|---|
| Notional value | $5M per order | `NOTIONAL_LIMIT_EXCEEDED` |
| Net position | 10,000 shares | `POSITION_LIMIT_EXCEEDED` |
| Self-trade | Same account both sides | `SELF_TRADE_PREVENTION` |
| Duplicate order ID | Idempotency key | `DUPLICATE_ORDER_ID` |
| Fat finger | Price >10% from last trade | `FAT_FINGER_PRICE_DEVIATION` |
| Circuit breaker | Symbol halted | `CIRCUIT_BREAKER_HALT` |

---

### 5. Circuit Breaker

Modelled on **SEC Rule 15c3-5** (Market Access Rule):

```java
// Halt if price moves more than 5% in 60 seconds
if (pctMove.compareTo(HALT_THRESHOLD_PCT) > 0) {
    haltSymbol(symbol, "Price moved " + pctMove + "% in 60s");
}

// Global kill switch: volatile boolean (readable on hot path without locks)
private volatile boolean halted = false;
```

---

## Performance Results

Run the simulation locally:
```bash
cd matching-engine
mvn package -DskipTests
java -jar target/titanx-engine.jar com.titanx.simulation.OrderGenerator
```

Expected output:
```
=================================================
  TitanX Simulation Results
=================================================
  Total Orders:    30,000
  Throughput:      1,667 orders/sec
  Fill Rate:       62.3%

  Matching Latency (µs):
    p50  =     12 µs
    p95  =     48 µs
    p99  =    127 µs
    max  =    891 µs
=================================================
```

---

## Running Locally

### Prerequisites
- Java 21+
- Maven 3.9+
- Docker Desktop

### Start Infrastructure
```bash
cd infra
docker compose up -d

# Wait for health checks
docker compose ps
# postgres: healthy, redis: healthy
```

### Run Matching Engine
```bash
cd matching-engine
mvn package -DskipTests
java -jar target/titanx-engine.jar
```

### Run API Gateway
```bash
cd api-gateway
mvn spring-boot:run
```

### Submit a Test Order
```bash
# Get a JWT token
curl -X POST http://localhost:8081/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"accountId": "acc-retail-01", "password": "titan"}'

# Submit a limit buy order
curl -X POST http://localhost:8081/api/v1/orders \
  -H "Authorization: Bearer <TOKEN>" \
  -H "Content-Type: application/json" \
  -d '{
    "symbol": "AAPL",
    "side": "BUY",
    "type": "LIMIT",
    "price": "150.00",
    "quantity": 100
  }'

# Check order book depth
curl http://localhost:8081/api/v1/orderbook/AAPL?depth=5
```

### View Dashboards
- **Grafana**: http://localhost:3000 (admin / titanx)
- **Prometheus**: http://localhost:9090

---

## What I'd Build Next

In a Goldman Strats interview, the answer to *"what would you improve?"* is:

1. **Disruptor ring buffer**: Replace `ReentrantReadWriteLock` with an LMAX Disruptor — a single-threaded event loop consuming from a lock-free ring buffer. This eliminates lock contention and achieves sub-microsecond latency.

2. **Snapshot + Replay**: Currently, crash recovery requires replaying all events from genesis. Add periodic snapshots of the order book state so recovery only replays events since the last snapshot.

3. **FIX Protocol gateway**: Replace the REST API with a FIX 4.4 session layer so institutional clients can connect with their existing infrastructure.

4. **Co-location optimization**: Pin the matching thread to a dedicated CPU core, use `Unsafe.compareAndSwapLong` for atomic quantity updates, and pre-allocate order objects to avoid GC pauses.

---

## Tech Stack

| Component | Technology | Why |
|---|---|---|
| Core Engine | Java 21 | Virtual threads, records, sealed types |
| Concurrency | `ReentrantReadWriteLock` per symbol | Per-instrument isolation |
| Order Book | `TreeMap` + `PriorityQueue` | Sorted price levels, FIFO within level |
| Cache | Redis 7.2 (Lettuce) | Sub-millisecond position reads |
| Persistence | PostgreSQL 16 (JDBC) | Append-only event sourcing |
| API | Spring Boot 3 (REST + WebSocket) | JWT auth, rate limiting |
| Security | JJWT, Token Bucket | 15-min tokens, 100 orders/sec limit |
| Metrics | Micrometer + Prometheus + Grafana | p50/p95/p99 latency heatmaps |
| Infrastructure | Docker Compose | One-command startup |
| Tests | JUnit 5, AssertJ | Correctness + concurrency verification |

---

## Project Structure

```
titanx/
├── matching-engine/                   # Core engine (Java 21)
│   └── src/main/java/com/titanx/
│       ├── model/                     # Order, Trade, OrderType, OrderStatus
│       ├── engine/                    # OrderBook, MatchingEngine
│       ├── risk/                      # RiskEngine, CircuitBreaker, RiskResult
│       ├── event/                     # OrderEvent, FillEvent, EventType
│       ├── persistence/               # EventStore (PostgreSQL), RedisPositionCache
│       └── simulation/                # OrderGenerator (load test)
│   └── src/test/java/com/titanx/
│       ├── engine/OrderBookTest.java  # 12 correctness tests
│       └── risk/RiskEngineTest.java   # 7 risk validation tests
│
├── api-gateway/                       # Spring Boot 3 (REST + WebSocket)
│   └── src/main/java/com/titanx/gateway/
│       ├── security/                  # JwtTokenService, TokenBucketRateLimiter
│       └── controller/                # OrderController (REST)
│
└── infra/
    ├── docker-compose.yml             # Redis, Postgres, Prometheus, Grafana
    ├── schema.sql                     # Append-only event schema
    └── prometheus.yml                 # Metrics scraping config
```

---

*Built to demonstrate systems-level thinking in the same problem space as Goldman Sachs Strats, Citadel, and LMAX.*
