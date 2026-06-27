#include "titanx/model/Trade.hpp"
#include <sstream>

namespace titanx {

Trade Trade::create(const Order& aggressor,
                    const Order& passive,
                    int64_t fillPrice,
                    int64_t fillQty) {
    return Trade{
        generate_uuid(),
        aggressor.symbol,
        aggressor.orderId,
        aggressor.accountId,
        passive.orderId,
        passive.accountId,
        fillPrice,
        fillQty,
        aggressor.side,
        SystemClock::now()
    };
}

nlohmann::json Trade::to_json() const {
    nlohmann::json j;
    j["tradeId"]            = tradeId;
    j["symbol"]             = symbol;
    j["aggressorOrderId"]   = aggressorOrderId;
    j["aggressorAccountId"] = aggressorAccountId;
    j["passiveOrderId"]     = passiveOrderId;
    j["passiveAccountId"]   = passiveAccountId;
    j["price"]              = price_to_string(price);
    j["quantity"]           = quantity;
    j["aggressorSide"]      = titanx::to_string(aggressorSide);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        executedAt.time_since_epoch()).count();
    j["executedAt"] = ms;
    return j;
}

std::string Trade::to_string() const {
    std::ostringstream oss;
    oss << "Trade{" << symbol
        << " " << titanx::to_string(aggressorSide)
        << " qty=" << quantity
        << " @ " << price_to_string(price)
        << ", aggressor=" << aggressorOrderId.substr(0, 8)
        << ", passive=" << passiveOrderId.substr(0, 8)
        << "}";
    return oss.str();
}

} // namespace titanx
