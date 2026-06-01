-- ============================================================
-- TitanX Database Schema — Event Sourcing + Audit Trail
-- ============================================================
-- Run against PostgreSQL 15+
-- docker exec -i titanx-postgres psql -U titanx < schema.sql

-- ---------------------------------------------------------------
--  ORDER EVENTS (append-only, event sourcing core)
-- ---------------------------------------------------------------
-- Every state change generates a row. We never UPDATE or DELETE.
-- Current order state = aggregate over events for that orderId.
-- This gives us:
--   1. Full audit trail (MiFID II, RegNMS compliant)
--   2. Crash recovery via event replay
--   3. Time-travel: "what was order X at 14:32:07.123456?"

CREATE TABLE IF NOT EXISTS order_events (
    id           BIGSERIAL PRIMARY KEY,
    event_id     UUID         NOT NULL UNIQUE,
    event_type   VARCHAR(50)  NOT NULL,
    order_id     UUID         NOT NULL,
    account_id   VARCHAR(100) NOT NULL,
    symbol       VARCHAR(20)  NOT NULL,
    payload      JSONB        NOT NULL,  -- full OrderEvent snapshot
    occurred_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

-- Indexes for common query patterns
CREATE INDEX IF NOT EXISTS idx_order_events_order_id     ON order_events (order_id);
CREATE INDEX IF NOT EXISTS idx_order_events_account_id   ON order_events (account_id);
CREATE INDEX IF NOT EXISTS idx_order_events_symbol_time  ON order_events (symbol, occurred_at DESC);
CREATE INDEX IF NOT EXISTS idx_order_events_type         ON order_events (event_type);

-- ---------------------------------------------------------------
--  TRADES (materialized fills for P&L and reporting)
-- ---------------------------------------------------------------
-- Denormalized from order_events for fast P&L queries.
-- The source of truth is still order_events; this is a projection.

CREATE TABLE IF NOT EXISTS trades (
    id                 BIGSERIAL PRIMARY KEY,
    trade_id           UUID          NOT NULL UNIQUE,
    symbol             VARCHAR(20)   NOT NULL,
    aggressor_order_id UUID          NOT NULL,
    aggressor_acct     VARCHAR(100)  NOT NULL,
    passive_order_id   UUID          NOT NULL,
    passive_acct       VARCHAR(100)  NOT NULL,
    fill_price         NUMERIC(18,8) NOT NULL,
    fill_quantity      BIGINT        NOT NULL,
    aggressor_side     VARCHAR(4)    NOT NULL CHECK (aggressor_side IN ('BUY','SELL')),
    notional_usd       NUMERIC(20,2) GENERATED ALWAYS AS (fill_price * fill_quantity) STORED,
    executed_at        TIMESTAMPTZ   NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_trades_symbol_time ON trades (symbol, executed_at DESC);
CREATE INDEX IF NOT EXISTS idx_trades_aggressor   ON trades (aggressor_acct);
CREATE INDEX IF NOT EXISTS idx_trades_passive     ON trades (passive_acct);

-- ---------------------------------------------------------------
--  ACCOUNTS (basic account registry)
-- ---------------------------------------------------------------

CREATE TABLE IF NOT EXISTS accounts (
    account_id       VARCHAR(100) PRIMARY KEY,
    display_name     VARCHAR(255) NOT NULL,
    permissions      TEXT[]       NOT NULL DEFAULT ARRAY['READ', 'TRADE'],
    max_position     BIGINT       NOT NULL DEFAULT 10000,
    max_notional_usd NUMERIC(20,2) NOT NULL DEFAULT 5000000,
    created_at       TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    is_active        BOOLEAN      NOT NULL DEFAULT TRUE
);

-- ---------------------------------------------------------------
--  POSITIONS (end-of-day reconciliation view)
-- ---------------------------------------------------------------
-- Positions in Redis are the hot-path. This view is for EOD reconciliation
-- and audit — checking Redis matches the event-sourced positions.

CREATE MATERIALIZED VIEW IF NOT EXISTS account_positions AS
    SELECT
        t.aggressor_acct   AS account_id,
        t.symbol,
        SUM(CASE WHEN t.aggressor_side = 'BUY'  THEN  t.fill_quantity ELSE 0 END)
        - SUM(CASE WHEN t.aggressor_side = 'SELL' THEN t.fill_quantity ELSE 0 END) AS net_position,
        SUM(CASE WHEN t.aggressor_side = 'BUY'  THEN  t.notional_usd  ELSE 0 END)
        - SUM(CASE WHEN t.aggressor_side = 'SELL' THEN t.notional_usd  ELSE 0 END) AS net_notional,
        MAX(t.executed_at)  AS last_trade_at
    FROM trades t
    GROUP BY t.aggressor_acct, t.symbol;

-- Refresh this view via: REFRESH MATERIALIZED VIEW CONCURRENTLY account_positions;
CREATE UNIQUE INDEX IF NOT EXISTS idx_acct_positions_pk
    ON account_positions (account_id, symbol);

-- ---------------------------------------------------------------
--  CIRCUIT BREAKER LOG
-- ---------------------------------------------------------------

CREATE TABLE IF NOT EXISTS circuit_breaker_events (
    id          BIGSERIAL PRIMARY KEY,
    symbol      VARCHAR(20)  NOT NULL,
    action      VARCHAR(20)  NOT NULL CHECK (action IN ('HALT', 'RESUME')),
    reason      TEXT,
    triggered_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- ---------------------------------------------------------------
--  HELPER: Replay order book for a symbol at a given timestamp
-- ---------------------------------------------------------------
-- Usage: SELECT * FROM order_events WHERE symbol = 'AAPL'
--          AND occurred_at <= '2024-01-15 14:32:07'
--          ORDER BY id ASC;
-- The Java replay logic processes this stream and reconstructs
-- the OrderBook state as it was at that exact moment.

-- ---------------------------------------------------------------
--  TEST DATA
-- ---------------------------------------------------------------

INSERT INTO accounts (account_id, display_name) VALUES
    ('acc-market-maker-01', 'TitanX Market Maker 1'),
    ('acc-market-maker-02', 'TitanX Market Maker 2'),
    ('acc-institutional-01', 'Goldman Sachs Principal Trading'),
    ('acc-retail-01', 'Retail Client Alpha')
ON CONFLICT (account_id) DO NOTHING;
