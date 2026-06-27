#pragma once

#include "titanx/event/OrderEvent.hpp"

#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

// Forward declare libpq types to avoid including libpq-fe.h in the header
typedef struct pg_conn PGconn;

namespace titanx {

/**
 * ============================================================
 * TitanX EventStore — Event Sourcing Persistence (C++17)
 * ============================================================
 *
 * Appends every OrderEvent to the order_events table in PostgreSQL.
 *
 * Key design decisions:
 *
 * 1. APPEND-ONLY writes — no UPDATE or DELETE.
 *    Event sourcing: current state is a projection over the event log.
 *
 * 2. ASYNC persistence via a dedicated writer thread.
 *    The matching engine never waits for disk I/O.
 *    Events are pushed to a lock-free queue and drained by the writer.
 *
 * 3. Single writer thread guarantees chronological ordering of events
 *    (BIGSERIAL IDs match insertion order).
 */
class EventStore {
public:
    EventStore(const std::string& connString);
    ~EventStore();

    // Non-blocking — pushes to queue, writer thread drains
    void append(const OrderEvent& event);

    // Graceful shutdown
    void close();

private:
    PGconn* conn_;
    std::string connString_;

    // Writer thread + queue
    std::queue<OrderEvent> queue_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::thread writerThread_;
    std::atomic<bool> running_{true};

    void writerLoop();
    void writeToDb(const OrderEvent& event);
    bool connectToDb();
};

} // namespace titanx
