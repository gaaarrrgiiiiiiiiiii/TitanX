package com.titanx.model;

/**
 * Supported order types.
 *
 * MARKET    — execute immediately at best available price
 * LIMIT     — execute at specified price or better; rest in book if not matched
 * IOC       — Immediate-or-Cancel: fill what's available, cancel the rest
 * FOK       — Fill-or-Kill: fill completely or cancel entirely (no partial fills)
 * STOP_LIMIT — becomes a LIMIT order when stopPrice is touched
 */
public enum OrderType {
    MARKET,
    LIMIT,
    IOC,
    FOK,
    STOP_LIMIT
}
