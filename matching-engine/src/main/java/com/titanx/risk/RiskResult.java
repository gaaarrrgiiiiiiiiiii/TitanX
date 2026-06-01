package com.titanx.risk;

/**
 * Immutable result of a pre-trade risk check.
 */
public record RiskResult(boolean approved, String reason) {
    public static RiskResult approved()              { return new RiskResult(true, null); }
    public static RiskResult rejected(String reason) { return new RiskResult(false, reason); }
}
