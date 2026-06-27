#include "titanx/engine/MatchingEngine.hpp"
#include "titanx/risk/RiskEngine.hpp"
#include "titanx/risk/CircuitBreaker.hpp"

#include <spdlog/spdlog.h>
#include <thread>
#include <vector>
#include <random>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <atomic>
#include <mutex>
#include <iostream>
#include <iomanip>

/**
 * ============================================================
 * TitanX OrderGenerator — Load Test Simulation (C++17)
 * ============================================================
 *
 * Simulates realistic market activity:
 *   - 10 market makers submitting bid/ask limit orders (60%)
 *   - Retail traders submitting market orders (25%)
 *   - IOC / FOK order mix (15%)
 *
 * Collects per-order latency and reports percentiles.
 */

using namespace titanx;

static constexpr int      NUM_ORDERS       = 100'000;
static constexpr int      NUM_THREADS      = 4;
static constexpr int      ORDERS_PER_THREAD = NUM_ORDERS / NUM_THREADS;
static constexpr int64_t  BASE_PRICE       = price_to_fixed(150.0);   // $150.00
static constexpr int64_t  TICK_SIZE        = price_to_fixed(0.01);    // $0.01

static std::atomic<int> totalTrades{0};
static std::mutex latencyMutex;
static std::vector<long> allLatencies;

void runSimulation(MatchingEngine& engine, int threadId) {
    std::mt19937 rng(42 + threadId);
    std::uniform_real_distribution<> orderTypeDist(0.0, 1.0);
    std::uniform_int_distribution<int64_t> priceDist(-50, 50); // ±50 ticks
    std::uniform_int_distribution<int64_t> qtyDist(1, 500);
    std::uniform_int_distribution<int> sideDist(0, 1);

    std::vector<long> localLatencies;
    localLatencies.reserve(ORDERS_PER_THREAD);

    for (int i = 0; i < ORDERS_PER_THREAD; ++i) {
        double roll = orderTypeDist(rng);
        OrderType type;
        int64_t price = 0;

        if (roll < 0.60) {
            type = OrderType::LIMIT;
            price = BASE_PRICE + priceDist(rng) * TICK_SIZE;
        } else if (roll < 0.85) {
            type = OrderType::MARKET;
        } else if (roll < 0.93) {
            type = OrderType::IOC;
            price = BASE_PRICE + priceDist(rng) * TICK_SIZE;
        } else {
            type = OrderType::FOK;
            price = BASE_PRICE + priceDist(rng) * TICK_SIZE;
        }

        OrderSide side = sideDist(rng) == 0 ? OrderSide::BUY : OrderSide::SELL;
        int64_t qty = qtyDist(rng);
        std::string accountId = "MM-" + std::to_string(threadId);

        Order order = Order::create(accountId, "AAPL", side, type,
                                    price, 0, qty);

        auto start = std::chrono::steady_clock::now();
        auto trades = engine.submit(order);
        auto end = std::chrono::steady_clock::now();

        long latencyMicros = std::chrono::duration_cast<std::chrono::microseconds>(
            end - start).count();
        localLatencies.push_back(latencyMicros);
        totalTrades.fetch_add(static_cast<int>(trades.size()),
                              std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> lock(latencyMutex);
        allLatencies.insert(allLatencies.end(),
                            localLatencies.begin(), localLatencies.end());
    }
}

int main() {
    spdlog::set_level(spdlog::level::warn); // suppress info during simulation
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    std::cout << "================================================\n"
              << "  TitanX Load Test Simulation (C++)\n"
              << "  " << NUM_ORDERS << " orders, "
              << NUM_THREADS << " threads, AAPL\n"
              << "================================================\n\n";

    // Build engine (no persistence — pure in-memory test)
    CircuitBreaker circuitBreaker;
    RiskEngine riskEngine(circuitBreaker);
    MatchingEngine engine(riskEngine);

    auto startTime = std::chrono::steady_clock::now();

    // Launch threads
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back(runSimulation, std::ref(engine), t);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto endTime = std::chrono::steady_clock::now();
    auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime).count();

    // ---- Compute percentiles ----
    std::sort(allLatencies.begin(), allLatencies.end());

    auto percentile = [&](double p) -> long {
        size_t idx = static_cast<size_t>(p * allLatencies.size());
        if (idx >= allLatencies.size()) idx = allLatencies.size() - 1;
        return allLatencies[idx];
    };

    double avgLatency = static_cast<double>(
        std::accumulate(allLatencies.begin(), allLatencies.end(), 0LL))
        / allLatencies.size();

    double throughput = static_cast<double>(NUM_ORDERS) * 1000.0 / totalMs;

    // ---- Report ----
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Simulation complete!\n\n";
    std::cout << "  Total orders:     " << NUM_ORDERS << "\n";
    std::cout << "  Total trades:     " << totalTrades.load() << "\n";
    std::cout << "  Elapsed time:     " << totalMs << " ms\n";
    std::cout << "  Throughput:       " << throughput << " orders/sec\n\n";

    std::cout << "  Latency Distribution (µs):\n";
    std::cout << "    Min:    " << allLatencies.front() << "\n";
    std::cout << "    P50:    " << percentile(0.50) << "\n";
    std::cout << "    P95:    " << percentile(0.95) << "\n";
    std::cout << "    P99:    " << percentile(0.99) << "\n";
    std::cout << "    P99.9:  " << percentile(0.999) << "\n";
    std::cout << "    Max:    " << allLatencies.back() << "\n";
    std::cout << "    Avg:    " << avgLatency << "\n\n";

    // Book snapshot
    auto snapshot = engine.getSnapshot("AAPL", 5);
    if (snapshot) {
        std::cout << "  Order Book Snapshot (AAPL, top 5 levels):\n";
        std::cout << "    Bids:\n";
        for (const auto& lvl : snapshot->bids) {
            std::cout << "      " << price_to_string(lvl.price)
                      << "  qty=" << lvl.totalQuantity
                      << "  orders=" << lvl.orderCount << "\n";
        }
        std::cout << "    Asks:\n";
        for (const auto& lvl : snapshot->asks) {
            std::cout << "      " << price_to_string(lvl.price)
                      << "  qty=" << lvl.totalQuantity
                      << "  orders=" << lvl.orderCount << "\n";
        }
    }

    std::cout << "\n================================================\n";
    return 0;
}
