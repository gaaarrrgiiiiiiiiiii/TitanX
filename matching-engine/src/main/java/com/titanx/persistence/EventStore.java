package com.titanx.persistence;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.titanx.event.OrderEvent;
import com.zaxxer.hikari.HikariConfig;
import com.zaxxer.hikari.HikariDataSource;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.SQLException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * ============================================================
 * TitanX EventStore — Phase 3: Event Sourcing Persistence
 * ============================================================
 *
 * Appends every OrderEvent to the `order_events` table in PostgreSQL.
 *
 * Key design decisions:
 *
 * 1. APPEND-ONLY writes — no UPDATE or DELETE.
 *    This implements the event sourcing pattern: current state is a
 *    projection over the event log. You can replay from any point in time.
 *
 * 2. ASYNC persistence via a dedicated single-threaded executor.
 *    The matching engine never waits for disk I/O. Events are written
 *    asynchronously to avoid adding latency to the hot path.
 *    This is a trade-off: in a crash, the last ~ms of events may be lost.
 *    Production systems use synchronous writes (sacrificing latency) or
 *    write-ahead logs + acknowledgement (LMAX approach).
 *
 * 3. HikariCP connection pool with small max connections (3).
 *    The async executor uses at most 1 thread per write — pool=3 gives
 *    headroom for concurrent audit queries without wasting connections.
 *
 * Schema: see infra/schema.sql
 */
public class EventStore {

    private static final Logger log = LoggerFactory.getLogger(EventStore.class);

    private final HikariDataSource dataSource;
    private final ObjectMapper objectMapper = new ObjectMapper()
            .findAndRegisterModules();  // JavaTimeModule for Instant serialization

    // Single-threaded executor: events are totally ordered in the DB
    private final ExecutorService writer = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "event-store-writer");
        t.setDaemon(true);
        return t;
    });

    private static final String INSERT_SQL = """
            INSERT INTO order_events
              (event_id, event_type, order_id, account_id, symbol, payload, occurred_at)
            VALUES (?, ?, ?, ?, ?, ?::jsonb, ?)
            """;

    public EventStore(String jdbcUrl, String username, String password) {
        HikariConfig config = new HikariConfig();
        config.setJdbcUrl(jdbcUrl);
        config.setUsername(username);
        config.setPassword(password);
        config.setMaximumPoolSize(3);
        config.setMinimumIdle(1);
        config.setConnectionTimeout(5000);
        config.setPoolName("titanx-eventstore");
        this.dataSource = new HikariDataSource(config);
        log.info("EventStore initialized with JDBC URL: {}", jdbcUrl);
    }

    /**
     * Asynchronously persist an event. Non-blocking from the caller's perspective.
     */
    public void append(OrderEvent event) {
        writer.submit(() -> writeToDb(event));
    }

    private void writeToDb(OrderEvent event) {
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(INSERT_SQL)) {

            ps.setString(1, event.eventId());
            ps.setString(2, event.type().name());
            ps.setString(3, event.orderId());
            ps.setString(4, event.accountId());
            ps.setString(5, event.symbol());
            ps.setString(6, objectMapper.writeValueAsString(event));
            ps.setObject(7, event.occurredAt());
            ps.executeUpdate();

        } catch (SQLException | com.fasterxml.jackson.core.JsonProcessingException e) {
            // In production: dead-letter queue + alerting
            log.error("Failed to persist event {} of type {}: {}",
                    event.eventId(), event.type(), e.getMessage());
        }
    }

    public void close() {
        writer.shutdown();
        dataSource.close();
    }
}
