#include "titanx/model/Order.hpp"
#include <random>
#include <sstream>
#include <iomanip>
#include <thread>

namespace titanx {

// ---------------------------------------------------------------
//  UUID Generation (simplified — no external dependency)
// ---------------------------------------------------------------

std::string generate_uuid() {
    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count() ^
        std::hash<std::thread::id>{}(std::this_thread::get_id())
    );

    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(rng);
    uint64_t b = dist(rng);

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<uint32_t>(a >> 32),
        static_cast<uint16_t>((a >> 16) & 0xFFFF),
        static_cast<uint16_t>(0x4000 | ((a & 0x0FFF))),   // version 4
        static_cast<uint16_t>(0x8000 | ((b >> 48) & 0x3FFF)), // variant
        static_cast<unsigned long long>(b & 0x0000FFFFFFFFFFFF));
    return std::string(buf);
}

// ---------------------------------------------------------------
//  Factory
// ---------------------------------------------------------------

Order Order::create(const std::string& accountId,
                    const std::string& symbol,
                    OrderSide side,
                    OrderType type,
                    int64_t price,
                    int64_t stopPrice,
                    int64_t quantity) {
    auto now = SystemClock::now();
    return Order{
        generate_uuid(),
        accountId,
        symbol,
        side,
        type,
        price,
        stopPrice,
        quantity,
        quantity,             // remainingQuantity starts == quantity
        OrderStatus::PENDING,
        now,
        now
    };
}

// ---------------------------------------------------------------
//  Copy-on-modify
// ---------------------------------------------------------------

Order Order::withRemainingQuantity(int64_t newRemaining) const {
    OrderStatus newStatus;
    if (newRemaining == 0) {
        newStatus = OrderStatus::FILLED;
    } else if (newRemaining < quantity) {
        newStatus = OrderStatus::PARTIALLY_FILLED;
    } else {
        newStatus = status;
    }

    Order copy = *this;
    copy.remainingQuantity = newRemaining;
    copy.status = newStatus;
    copy.updatedAt = SystemClock::now();
    return copy;
}

Order Order::withStatus(OrderStatus newStatus) const {
    Order copy = *this;
    copy.status = newStatus;
    copy.updatedAt = SystemClock::now();
    return copy;
}

// ---------------------------------------------------------------
//  Serialization
// ---------------------------------------------------------------

nlohmann::json Order::to_json() const {
    nlohmann::json j;
    j["orderId"]           = orderId;
    j["accountId"]         = accountId;
    j["symbol"]            = symbol;
    j["side"]              = titanx::to_string(side);
    j["type"]              = titanx::to_string(type);
    j["price"]             = price_to_string(price);
    j["stopPrice"]         = price_to_string(stopPrice);
    j["quantity"]          = quantity;
    j["remainingQuantity"] = remainingQuantity;
    j["status"]            = titanx::to_string(status);

    auto createdMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        createdAt.time_since_epoch()).count();
    auto updatedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        updatedAt.time_since_epoch()).count();
    j["createdAt"] = createdMs;
    j["updatedAt"] = updatedMs;
    return j;
}

std::string Order::to_string() const {
    std::ostringstream oss;
    oss << "Order{id=" << orderId.substr(0, 8)
        << ", acct=" << accountId
        << ", " << titanx::to_string(side)
        << " " << titanx::to_string(type)
        << " " << symbol
        << " qty=" << remainingQuantity << "/" << quantity
        << " @ " << (type == OrderType::MARKET ? "MKT" : price_to_string(price))
        << "}";
    return oss.str();
}

} // namespace titanx
