#include "titanx/gateway/JwtTokenService.hpp"

#include <jwt-cpp/jwt.h>
#include <spdlog/spdlog.h>

namespace titanx {

JwtTokenService::JwtTokenService() {}

std::string JwtTokenService::generateAccessToken(
        const std::string& accountId,
        const std::vector<std::string>& permissions) {

    auto now = std::chrono::system_clock::now();

    // Build permissions as a comma-separated string for jwt-cpp
    // (jwt-cpp doesn't natively support array claims easily)
    std::string permsStr;
    for (size_t i = 0; i < permissions.size(); ++i) {
        if (i > 0) permsStr += ",";
        permsStr += permissions[i];
    }

    auto token = jwt::create()
        .set_issuer("titanx")
        .set_subject(accountId)
        .set_payload_claim("accountId", jwt::claim(accountId))
        .set_payload_claim("permissions", jwt::claim(permsStr))
        .set_payload_claim("tokenType", jwt::claim(std::string("ACCESS")))
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(ACCESS_TOKEN_TTL_SECONDS))
        .sign(jwt::algorithm::hs256{signingSecret_});

    return token;
}

std::string JwtTokenService::generateRefreshToken(
        const std::string& accountId,
        const std::string& sessionId) {

    auto now = std::chrono::system_clock::now();

    auto token = jwt::create()
        .set_issuer("titanx")
        .set_subject(accountId)
        .set_payload_claim("sessionId", jwt::claim(sessionId))
        .set_payload_claim("tokenType", jwt::claim(std::string("REFRESH")))
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(REFRESH_TOKEN_TTL_SECONDS))
        .sign(jwt::algorithm::hs256{signingSecret_});

    return token;
}

JwtClaims JwtTokenService::validateAndExtract(const std::string& token) {
    JwtClaims claims;

    try {
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{signingSecret_})
            .with_issuer("titanx");

        auto decoded = jwt::decode(token);
        verifier.verify(decoded);

        claims.accountId = decoded.get_subject();
        claims.tokenType = decoded.get_payload_claim("tokenType").as_string();

        // Parse permissions from comma-separated string
        if (decoded.has_payload_claim("permissions")) {
            std::string permsStr = decoded.get_payload_claim("permissions").as_string();
            std::string perm;
            for (char c : permsStr) {
                if (c == ',') {
                    if (!perm.empty()) claims.permissions.push_back(perm);
                    perm.clear();
                } else {
                    perm += c;
                }
            }
            if (!perm.empty()) claims.permissions.push_back(perm);
        }

        claims.valid = true;

    } catch (const std::exception& e) {
        claims.valid = false;
        claims.error = e.what();
        spdlog::debug("JWT validation failed: {}", e.what());
    }

    return claims;
}

bool JwtTokenService::isExpired(const std::string& token) {
    try {
        auto decoded = jwt::decode(token);
        auto exp = decoded.get_expires_at();
        return exp < std::chrono::system_clock::now();
    } catch (...) {
        return true;
    }
}

} // namespace titanx
