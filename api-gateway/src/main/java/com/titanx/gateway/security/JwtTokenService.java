package com.titanx.gateway.security;

import io.jsonwebtoken.*;
import io.jsonwebtoken.security.Keys;
import org.springframework.stereotype.Component;

import javax.crypto.SecretKey;
import java.time.Instant;
import java.util.Date;
import java.util.List;

/**
 * ============================================================
 * TitanX JwtTokenService — Phase 4: Security
 * ============================================================
 *
 * JWT strategy:
 *  - Access token: 15-minute expiry (short-lived, as required for trading systems)
 *  - Refresh token: 24-hour expiry, rotated on use
 *  - Claims: accountId, permissions (READ/TRADE/ADMIN), sessionId
 *
 * Why 15-minute access tokens?
 *  "Longer tokens are a security risk — a compromised token could be
 *   used to submit orders until it expires. 15 minutes limits the blast
 *   radius. The refresh token is rotated on every use (rotation prevents
 *   replay attacks)."
 *
 * Algorithm: HS256 with 256-bit secret key.
 * In production: use RS256 with a KMS-managed private key, so the
 * key material never leaves the HSM.
 */
@Component
public class JwtTokenService {

    private static final long ACCESS_TOKEN_TTL_SECONDS  = 15 * 60;        // 15 minutes
    private static final long REFRESH_TOKEN_TTL_SECONDS = 24 * 60 * 60;   // 24 hours

    // In production: load from environment variable or AWS Secrets Manager
    private final SecretKey signingKey = Keys.hmacShaKeyFor(
            "titanx-secret-key-minimum-256-bits-for-hs256!!".getBytes());

    public String generateAccessToken(String accountId, List<String> permissions) {
        Instant now = Instant.now();
        return Jwts.builder()
                .subject(accountId)
                .claim("accountId", accountId)
                .claim("permissions", permissions)
                .claim("tokenType", "ACCESS")
                .issuedAt(Date.from(now))
                .expiration(Date.from(now.plusSeconds(ACCESS_TOKEN_TTL_SECONDS)))
                .signWith(signingKey)
                .compact();
    }

    public String generateRefreshToken(String accountId, String sessionId) {
        Instant now = Instant.now();
        return Jwts.builder()
                .subject(accountId)
                .claim("sessionId", sessionId)
                .claim("tokenType", "REFRESH")
                .issuedAt(Date.from(now))
                .expiration(Date.from(now.plusSeconds(REFRESH_TOKEN_TTL_SECONDS)))
                .signWith(signingKey)
                .compact();
    }

    public Claims validateAndExtract(String token) {
        return Jwts.parser()
                .verifyWith(signingKey)
                .build()
                .parseSignedClaims(token)
                .getPayload();
    }

    public boolean isExpired(String token) {
        try {
            Claims claims = validateAndExtract(token);
            return claims.getExpiration().before(new Date());
        } catch (JwtException e) {
            return true;
        }
    }

    @SuppressWarnings("unchecked")
    public List<String> extractPermissions(Claims claims) {
        return (List<String>) claims.get("permissions", List.class);
    }
}
