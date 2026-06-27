#pragma once

#include "titanx/model/Order.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace titanx {

/**
 * Represents a completed trade between two orders.
 * A single aggressive order can generate multiple Trade records
 * if it crosses multiple price levels.
 *
 * Aggressor: the order that CAME IN and crossed the spread.
 * Passive:   the order RESTING in the book.
 * Fill price = passive order's limit price (aggressor accepts what the book offers).
 */
struct Trade {
    std::string   tradeId;
    std::string   symbol;
    std::string   aggressorOrderId;
    std::string   aggressorAccountId;
    std::string   passiveOrderId;
    std::string   passiveAccountId;
    int64_t       price;             // fixed-point fill price
    int64_t       quantity;          // shares exchanged
    OrderSide     aggressorSide;
    SysTimePoint  executedAt;

    // Factory method
    static Trade create(const Order& aggressor,
                        const Order& passive,
                        int64_t fillPrice,
                        int64_t fillQty);

    // Notional value = price * quantity (integer result in fixed-point)
    int64_t notional() const { return price * quantity / PRICE_SCALE; }

    nlohmann::json to_json() const;
    std::string to_string() const;
};

} // namespace titanx
