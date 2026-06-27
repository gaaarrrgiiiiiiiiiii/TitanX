#pragma once

#include <string>

namespace titanx {

/**
 * Result of a pre-trade risk check.
 * Header-only for simplicity.
 */
struct RiskResult {
    bool        approved;
    std::string reason;

    static RiskResult approve() {
        return {true, ""};
    }

    static RiskResult rejected(const std::string& reason) {
        return {false, reason};
    }
};

} // namespace titanx
