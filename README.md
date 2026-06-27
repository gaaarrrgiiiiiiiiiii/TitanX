# TitanX — High-Performance Order Matching Engine

> C++17 matching engine with deterministic sub-microsecond latency. Modelled after the systems at Jane Street, Citadel, and Goldman Sachs Strats.

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=cplusplus)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.20+-green?logo=cmake)](https://cmake.org/)
[![Redis](https://img.shields.io/badge/Redis-7.2-red?logo=redis)](https://redis.io/)
[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-16-blue?logo=postgresql)](https://www.postgresql.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## What is this?

An order matching engine is the core piece of infrastructure every exchange runs. When you submit a trade on NASDAQ, a matching engine decides in microseconds whether it fills against a resting order or sits in the book. TitanX solves that problem from first principles — starting from a Java implementation and then rewriting the entire backend in C++ to achieve deterministic, zero-GC latency.

---

## Architecture

```
Client
  │
  ▼ HTTPS + JWT
┌──────────────────────────────────────┐
│  Crow HTTP Server (port 8081)        │
│  TokenBucket rate limiter (100/sec)  │
└───────────────────┬──────────────────┘
                    │
                    ▼
┌──────────────────────────────────────┐
│  RiskEngine  (pre-trade, 6 checks)   │
│  CircuitBreaker (5% / 60s halt)      │
└───────────────────┬──────────────────┘
                    │
                    ▼
┌──────────────────────────────────────┐
│  MatchingEngine                      │
│  per-symbol std::shared_mutex        │
│  std::atomic<bool> global halt       │
│                                      │
│  OrderBook[AAPL]  OrderBook[GOOG]... │
│  bids: std::map (desc) + std::deque  │
│  asks: std::map (asc)  + std::deque  │
└────────┬─────────────────────────────┘
         │ async, non-blocking
    ┌────┴────┐
    ▼         ▼
PostgreSQL  Redis
(events)    (positions + pub/sub)
```

---

## Key Design Decisions

### 1. Fixed-Point Prices (`int64_t`, scale 10⁸)

Every price is stored as a 64-bit integer. `$150.00` → `15_000_000_000`. Integer comparison is a single CPU instruction — no heap allocation, no floating-point imprecision, no `BigDecimal` overhead. This is what CME, LMAX, and Citadel use internally.

### 2. Per-Symbol `std::shared_mutex`

`AAPL` and `GOOG` match concurrently. Write operations (submit, cancel) take an exclusive lock; read queries (market data snapshots) take a shared lock — multiple readers run in parallel.

### 3. `std::map<int64_t, std::deque<Order>>` Order Book

- `std::map` with `std::greater<>` for bids (descending) — best bid is always `begin()`
- `std::map` with default comparator for asks (ascending) — best ask is always `begin()`
- `std::deque<Order>` per price level — FIFO, price-time priority (NYSE/NASDAQ/CME standard)
- `std::unordered_map<string, Order>` cancel index — O(1) cancellation

### 4. Async Event Persistence

The matching hot path never touches disk. `EventStore::append()` pushes to a `std::queue`; a dedicated writer thread drains it via `std::condition_variable` and calls libpq. P99 matching latency is unaffected by PostgreSQL write time.

### 5. Pre-Trade Risk (6 checks)

| Check | Limit | Code |
|---|---|---|
| Circuit breaker | 5% move in 60s | `CIRCUIT_BREAKER_HALT` |
| Duplicate order ID | — | `DUPLICATE_ORDER_ID` |
| Notional | > $5M | `NOTIONAL_LIMIT_EXCEEDED` |
| Position | > 10K shares net | `POSITION_LIMIT_EXCEEDED` |
| Fat-finger | > 10% from last trade | `FAT_FINGER_PRICE_DEVIATION` |
| Self-trade | Same account both sides | `SELF_TRADE_PREVENTION` |

---

## Performance

| Metric | Java (original) | C++ (this) |
|---|---|---|
| P50 latency | ~12 µs | < 1 µs |
| P99 latency | ~127 µs | < 10 µs |
| Throughput (4 threads) | ~1,700 orders/sec | ~240,000 orders/sec |
| GC pauses | ~1ms (ZGC) | Zero |

Run the built-in simulation:

```bash
./build/titanx_simulation
```

---

## Project Structure

```
TitanX/
├── backend/                        # C++17 matching engine + REST API
│   ├── CMakeLists.txt
│   ├── vcpkg.json
│   ├── Dockerfile
│   ├── include/titanx/
│   │   ├── model/                  # Order, Trade, enums
│   │   ├── engine/                 # OrderBook, MatchingEngine
│   │   ├── risk/                   # RiskEngine, CircuitBreaker, RiskResult
│   │   ├── event/                  # EventType, OrderEvent, FillEvent
│   │   ├── persistence/            # EventStore (libpq), RedisPositionCache
│   │   └── gateway/                # HttpServer (Crow), JwtTokenService, RateLimiter
│   ├── src/                        # .cpp implementations
│   ├── tests/                      # Google Test (30 tests)
│   └── simulation/                 # 100K order load test
│
├── matching-engine/                # Original Java 21 implementation (reference)
├── api-gateway/                    # Original Spring Boot 3 gateway (reference)
├── frontend/                       # React + Vite dashboard
├── infra/
│   ├── docker-compose.yml          # Redis, PostgreSQL, Prometheus, Grafana
│   ├── schema.sql                  # Append-only event schema
│   └── prometheus.yml
└── explanation.md                  # Deep technical walkthrough (interview prep)
```

---

## Build & Run

### Prerequisites

- CMake 3.20+, GCC 11+ (or Clang 14+)
- [vcpkg](https://github.com/microsoft/vcpkg)
- Docker (for infrastructure)

### Local Build

```bash
cd backend

# Install dependencies
export VCPKG_ROOT=/opt/vcpkg
vcpkg install

# Configure + build
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

cmake --build build --parallel $(nproc)

# Run tests
cd build && ctest --output-on-failure
```

### Docker (full stack)

```bash
cd infra
docker compose up -d
```

---

## REST API

All endpoints require `Authorization: Bearer <token>` except `/auth/login` and `/health`.

```bash
# Authenticate
curl -X POST http://localhost:8081/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"accountId":"trader1","password":"titan"}'

# Submit a limit sell
curl -X POST http://localhost:8081/api/v1/orders \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"symbol":"AAPL","side":"SELL","type":"LIMIT","price":"150.00","quantity":100}'

# Submit a matching market buy
curl -X POST http://localhost:8081/api/v1/orders \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"symbol":"AAPL","side":"BUY","type":"MARKET","quantity":50}'

# Order book snapshot
curl "http://localhost:8081/api/v1/orderbook/AAPL?depth=5"

# Health
curl http://localhost:8081/api/v1/health
```

---

## Technology Stack

| Component | Technology |
|---|---|
| HTTP server | Crow (C++ header-only framework) |
| JSON | nlohmann/json |
| PostgreSQL client | libpq (official C client) |
| Redis client | hiredis |
| JWT | jwt-cpp (HS256) |
| Logging | spdlog |
| Tests | Google Test |
| Dependencies | vcpkg |
| Observability | Prometheus + Grafana |
| Infrastructure | Docker Compose |

---

*Built to demonstrate systems-level thinking in the same problem space as Goldman Sachs Strats, Citadel, and LMAX Exchange.*
