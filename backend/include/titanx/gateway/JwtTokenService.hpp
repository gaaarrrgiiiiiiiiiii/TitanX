#pragma once

#include <string>
#include <vector>
#include <chrono>

namespace titanx {

/**
 * ============================================================
 * TitanX JwtTokenService — JWT Authentication (C++17)
 * ============================================================
 *
 * JWT strategy:
 *   - Access token: 15-minute expiry (short-lived for trading systems)
 *   - Refresh token: 24-hour expiry, rotated on use
 *   - Claims: accountId, permissions (READ/TRADE/ADMIN)
 *
 * Algorithm: HS256 with 256-bit secret key.
 * In production: use RS256 with a KMS-managed private key.
 */

struct JwtClaims {
    std::string              accountId;
    std::vector<std::string> permissions;
    std::string              tokenType; // "ACCESS" or "REFRESH"
    bool                     valid = false;
    std::string              error;
};

class JwtTokenService {
public:
    JwtTokenService();

    std::string generateAccessToken(const std::string& accountId,
                                    const std::vector<std::string>& permissions);
    std::string generateRefreshToken(const std::string& accountId,
                                     const std::string& sessionId);

    JwtClaims validateAndExtract(const std::string& token);
    bool isExpired(const std::string& token);

private:
    static constexpr int ACCESS_TOKEN_TTL_SECONDS  = 15 * 60;       // 15 minutes
    static constexpr int REFRESH_TOKEN_TTL_SECONDS  = 24 * 60 * 60; // 24 hours

    // In production: load from environment variable or secrets manager
    std::string signingSecret_ = "titanx-secret-key-minimum-256-bits-for-hs256!!";
};

} // namespace titanx
