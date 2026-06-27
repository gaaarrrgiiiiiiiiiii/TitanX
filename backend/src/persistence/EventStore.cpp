#include "titanx/persistence/EventStore.hpp"
#include <libpq-fe.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

namespace titanx {

EventStore::EventStore(const std::string& connString)
    : conn_(nullptr), connString_(connString) {

    if (!connectToDb()) {
        spdlog::warn("EventStore: initial DB connection failed, will retry on writes");
    }

    // Start the dedicated writer thread
    writerThread_ = std::thread(&EventStore::writerLoop, this);
    spdlog::info("EventStore initialized with connection: {}",
                 connString.substr(0, connString.find("password")));
}

EventStore::~EventStore() {
    close();
}

bool EventStore::connectToDb() {
    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }

    conn_ = PQconnectdb(connString_.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
        spdlog::error("EventStore: PostgreSQL connection failed: {}",
                      PQerrorMessage(conn_));
        PQfinish(conn_);
        conn_ = nullptr;
        return false;
    }
    return true;
}

void EventStore::append(const OrderEvent& event) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.push(event);
    }
    queueCV_.notify_one();
}

void EventStore::writerLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCV_.wait(lock, [this] {
            return !queue_.empty() || !running_.load(std::memory_order_relaxed);
        });

        // Drain all queued events
        while (!queue_.empty()) {
            OrderEvent event = std::move(queue_.front());
            queue_.pop();
            lock.unlock();

            writeToDb(event);

            lock.lock();
        }
    }

    // Drain remaining events on shutdown
    std::lock_guard<std::mutex> lock(queueMutex_);
    while (!queue_.empty()) {
        writeToDb(queue_.front());
        queue_.pop();
    }
}

void EventStore::writeToDb(const OrderEvent& event) {
    if (!conn_) {
        if (!connectToDb()) {
            spdlog::error("EventStore: cannot connect to DB, dropping event {}",
                          event.eventId);
            return;
        }
    }

    std::string payload = event.to_json().dump();

    // Timestamps as ISO 8601
    auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        event.occurredAt.time_since_epoch()).count();

    // PostgreSQL epoch (seconds + fractional)
    double epochSec = static_cast<double>(epochMs) / 1000.0;
    char timestampBuf[64];
    std::snprintf(timestampBuf, sizeof(timestampBuf),
                  "to_timestamp(%.3f)", epochSec);

    // Build parameterized query
    std::string sql =
        "INSERT INTO order_events "
        "(event_id, event_type, order_id, account_id, symbol, payload, occurred_at) "
        "VALUES ($1, $2, $3, $4, $5, $6::jsonb, " +
        std::string(timestampBuf) + ")";

    const char* paramValues[6] = {
        event.eventId.c_str(),
        titanx::to_string(event.type).c_str(),
        event.orderId.c_str(),
        event.accountId.c_str(),
        event.symbol.c_str(),
        payload.c_str()
    };

    PGresult* result = PQexecParams(conn_, sql.c_str(),
        6, nullptr, paramValues, nullptr, nullptr, 0);

    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        spdlog::error("EventStore: failed to persist event {} of type {}: {}",
                      event.eventId, titanx::to_string(event.type),
                      PQerrorMessage(conn_));
        // Try to reconnect for next write
        connectToDb();
    }

    PQclear(result);
}

void EventStore::close() {
    running_.store(false, std::memory_order_relaxed);
    queueCV_.notify_one();

    if (writerThread_.joinable()) {
        writerThread_.join();
    }

    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
}

} // namespace titanx
