cmake_minimum_required(VERSION 3.20)
project(titanx-engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
#  Dependencies (via vcpkg or system packages)
# ============================================================
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ============================================================
#  TitanX Core Library
# ============================================================
set(TITANX_SOURCES
    src/model/Order.cpp
    src/model/Trade.cpp
    src/engine/OrderBook.cpp
    src/engine/MatchingEngine.cpp
    src/risk/RiskEngine.cpp
    src/risk/CircuitBreaker.cpp
    src/event/OrderEvent.cpp
    src/persistence/EventStore.cpp
    src/persistence/RedisPositionCache.cpp
    src/gateway/HttpServer.cpp
    src/gateway/JwtTokenService.cpp
    src/gateway/TokenBucketRateLimiter.cpp
)

add_library(titanx_core STATIC ${TITANX_SOURCES})

target_include_directories(titanx_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(titanx_core PUBLIC
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Crow::Crow
    PostgreSQL::PostgreSQL
    hiredis::hiredis
    jwt-cpp::jwt-cpp
    prometheus-cpp::pull
    Threads::Threads
)

# ============================================================
#  Main Application
# ============================================================
add_executable(titanx_server src/main.cpp)
target_link_libraries(titanx_server PRIVATE titanx_core)

# ============================================================
#  Load Test Simulation
# ============================================================
add_executable(titanx_simulation src/simulation/OrderGenerator.cpp)
target_link_libraries(titanx_simulation PRIVATE titanx_core)

# ============================================================
#  Tests
# ============================================================
enable_testing()

add_executable(titanx_tests
    tests/OrderBookTest.cpp
    tests/RiskEngineTest.cpp
    tests/MatchingEngineTest.cpp
)

target_link_libraries(titanx_tests PRIVATE
    titanx_core
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(titanx_tests)
