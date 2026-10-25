#include "redisdb/server/RespCommandServer.hpp"

#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/commands/CommandErrors.hpp"
#include "redisdb/commands/ConnectivityCommands.hpp"
#include "redisdb/commands/HashCommands.hpp"
#include "redisdb/commands/KeyCommands.hpp"
#include "redisdb/commands/ListCommands.hpp"
#include "redisdb/commands/SetCommands.hpp"
#include "redisdb/commands/StringCommands.hpp"
#include "redisdb/net/Connection.hpp"
#include "redisdb/net/ConnectionManager.hpp"
#include "redisdb/net/TcpListener.hpp"
#include "redisdb/protocol/RespPipeline.hpp"
#include "redisdb/storage/Database.hpp"

#if defined(__APPLE__)
#include "redisdb/net/KqueuePoller.hpp"
#elif defined(__linux__)
#include "redisdb/net/EpollPoller.hpp"
#else
#error "RESP2 server requires macOS or Linux"
#endif

#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace redisdb::server {
namespace {

#if defined(__APPLE__)
using NativePoller = net::KqueuePoller;
#else
using NativePoller = net::EpollPoller;
#endif

volatile std::sig_atomic_t command_stop_requested = 0;

void request_command_stop(int) { command_stop_requested = 1; }

void install_command_signals() {
    if (std::signal(SIGINT, request_command_stop) == SIG_ERR ||
        std::signal(SIGTERM, request_command_stop) == SIG_ERR) {
        throw std::runtime_error("unable to install RESP2 shutdown signal handlers");
    }
}

// AOF's read fast path must use the same instant for the due-expiry check
// and execution. Otherwise a read can cross a deadline and remove a key
// outside a persistence transaction. Nonpersistent behavior stays unchanged.
class CommandClock final : public storage::Clock {
  public:
    explicit CommandClock(const storage::Clock& source) : source_(source) {}
    storage::MonotonicTimePoint monotonic_now() const noexcept override {
        return source_.monotonic_now();
    }
    storage::WallTimePoint wall_now() const noexcept override {
        return sample_ ? *sample_ : source_.wall_now();
    }
    class Scope {
      public:
        Scope(CommandClock& clock, bool enabled) : clock_(clock) {
            if (enabled)
                clock_.sample_ = clock_.source_.wall_now();
        }
        ~Scope() { clock_.sample_.reset(); }

      private:
        CommandClock& clock_;
    };

  private:
    const storage::Clock& source_;
    std::optional<storage::WallTimePoint> sample_;
};

} // namespace

void run_resp2_server(std::uint16_t port, std::size_t max_clients, storage::MemoryOptions memory,
                      persistence::AofOptions aof) {
    if (max_clients == 0) {
        throw std::invalid_argument("max_clients must be positive");
    }

    command_stop_requested = 0;
    NativePoller poller;
    net::ConnectionManager::Limits limits;
    limits.max_connections = max_clients;

    net::ConnectionManager manager(net::TcpListener::bind_loopback(port), poller, limits);
    // One database for all TCP connections; the single event-loop thread
    // serializes each command. It must outlive dispatcher route closures.
    storage::SystemClock server_clock;
    CommandClock command_clock(server_clock);
    storage::Database database(command_clock, memory);
    std::unique_ptr<persistence::AofLog> journal;
    if (aof.enabled) {
        journal = std::make_unique<persistence::AofLog>(aof, server_clock);
        journal->recover(database); // no readiness/command execution before complete validation
    }
    std::exception_ptr persistence_failure;
    auto routes = commands::connectivity_routes();
    for (auto& route : commands::string_routes(database)) {
        routes.push_back(std::move(route));
    }
    for (auto& route : commands::key_routes(database)) {
        routes.push_back(std::move(route));
    }
    for (auto& route : commands::list_routes(database)) {
        routes.push_back(std::move(route));
    }
    for (auto& route : commands::hash_routes(database)) {
        routes.push_back(std::move(route));
    }
    for (auto& route : commands::set_routes(database)) {
        routes.push_back(std::move(route));
    }
    const commands::CommandDispatcher dispatcher(std::move(routes));
    const protocol::RespPipeline pipeline;

    manager.set_resumable_input_handler([&](net::Connection& connection) {
        // RESP2 input is framed and decoded through the existing bounded pipeline.
        // QUIT requests a graceful close only after its +OK reply has been sent.
        const auto outcome = pipeline.drain(connection, [&](const protocol::RespValue& request) {
            if (persistence_failure) {
                std::rethrow_exception(persistence_failure);
            }
            CommandClock::Scope command_time(command_clock, journal != nullptr);
            auto decoded = commands::decode_command(request);
            if (!decoded.command) {
                return protocol::RespValue::error("ERR expected RESP2 array of bulk strings");
            }

            const auto& name = decoded.command->name();
            const bool mutating =
                name == "SET" || name == "MSET" || name == "APPEND" || name == "INCR" ||
                name == "DECR" || name == "INCRBY" || name == "DECRBY" || name == "DEL" ||
                name == "EXPIRE" || name == "PEXPIRE" || name == "PERSIST" || name == "LPUSH" ||
                name == "RPUSH" || name == "LPOP" || name == "RPOP" || name == "HSET" ||
                name == "HDEL" || name == "SADD" || name == "SREM";
            std::unique_ptr<storage::Database::Transaction> transaction;
            if (journal && (mutating || database.has_due_expiration())) {
                transaction = std::make_unique<storage::Database::Transaction>(database);
            }
            auto dispatched = dispatcher.dispatch(*decoded.command);
            if (!dispatched.handled) {
                if (dispatched.error != commands::DispatchError::UnknownCommand) {
                    throw std::logic_error("unexpected dispatcher outcome");
                }
                return commands::unknown_command_reply(*decoded.command);
            }

            if (transaction &&
                !std::holds_alternative<protocol::Error>(dispatched.handled->reply.storage())) {
                const auto effects = transaction->effects();
                try {
                    journal->append(effects); // always policy syncs here, before publication/reply
                } catch (...) {
                    persistence_failure = std::current_exception();
                    throw;
                }
                transaction->commit(); // allocation-free publication; errors roll back by RAII
            }

            if (dispatched.handled->disposition ==
                commands::ConnectionDisposition::CloseAfterReply) {
                connection.request_close_after_flush();
            }
            return std::move(dispatched.handled->reply);
        });

        using Action = net::ConnectionManager::InputAction;
        switch (outcome.stop) {
        case protocol::PipelineStop::NeedMore:
        case protocol::PipelineStop::Closing:
            return Action::AwaitRead;
        case protocol::PipelineStop::BudgetExhausted:
            return Action::Continue;
        case protocol::PipelineStop::Backpressured:
            return Action::AwaitWrite;
        case protocol::PipelineStop::Invalid:
            // Malformed RESP2 is isolated to this client by ConnectionManager.
            throw std::runtime_error("invalid RESP2 request frame");
        }
        throw std::logic_error("unexpected pipeline result");
    });

    install_command_signals();
    std::cout << "RESP_READY 127.0.0.1:" << manager.listener_port() << '\n' << std::flush;
    constexpr auto PollInterval = std::chrono::milliseconds{100};
    constexpr std::size_t MaxExpiryChecksPerTurn = 32;
    auto next_sweep = server_clock.monotonic_now();
    while (command_stop_requested == 0) {
        (void)manager.poll_once(PollInterval);
        // ConnectionManager isolates ordinary client exceptions. A journal
        // failure is process-fatal, including other callbacks in this poll turn.
        if (persistence_failure) {
            std::rethrow_exception(persistence_failure);
        }
        // Maintenance is outside each individual command but serialized on
        // the same event-loop thread: bounded work, no data races or timers.
        const auto current = server_clock.monotonic_now();
        if (current >= next_sweep) {
            if (journal && database.has_due_expiration()) {
                storage::Database::Transaction transaction(database);
                (void)database.sweep_expired(MaxExpiryChecksPerTurn);
                journal->append(transaction.effects());
                transaction.commit();
            } else if (!journal) {
                (void)database.sweep_expired(MaxExpiryChecksPerTurn);
            }
            next_sweep = current + PollInterval;
        }
        if (journal) {
            journal->tick();
        }
    }
    if (journal) {
        journal->finish();
    }
    std::cout << "RESP_STOPPED\n" << std::flush;
}

} // namespace redisdb::server
