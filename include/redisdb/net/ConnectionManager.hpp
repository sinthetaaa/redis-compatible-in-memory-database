#pragma once

#include "redisdb/net/Connection.hpp"
#include "redisdb/net/EventPoller.hpp"
#include "redisdb/net/TcpListener.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace redisdb::net {

// Single-threaded owner of accepted TCP connections.
//
// Owns:
//   - TcpListener
//   - Each accepted Connection
//
// Borrows:
//   - EventPoller, which must outlive this manager
//
// A connection is unregistered before its socket
// is closed.
//
// Phase 1F-B manages registration and ownership.
// Event dispatch is added in Phase 1F-C.
class ConnectionManager final {
  public:
    struct Limits {
        std::size_t max_connections = 1024;
        std::size_t max_accept_per_call = 32;
        std::size_t max_read_per_event = 8192;
        std::size_t max_write_per_event = 8192;
        // Bounded number of deferred clients per poll.
        std::size_t max_deferred_per_poll = 32;
        // Zero disables idle eviction.
        std::chrono::milliseconds idle_timeout{0};
        Connection::Limits connection{};
    };

    struct AcceptResult {
        // Newly accepted, successfully registered
        // connection identities.
        std::vector<RegistrationToken> tokens;

        bool would_block = false;
        bool capacity_reached = false;
    };

    struct DispatchResult {
        std::size_t accepted = 0;
        std::size_t bytes_received = 0;
        std::size_t bytes_sent = 0;
        std::size_t closed = 0;
        std::size_t stale_events = 0;
        // Additional records coalesced by token,
        // including records with distinct flags.
        std::size_t duplicate_events = 0;
        std::size_t handler_failures = 0;
        std::size_t output_limit_closes = 0;
        // Socket errors, excluding ordinary EOF.
        std::size_t io_failures = 0;
    };

    // A transport callback, not a protocol parser.
    // May consume input and queue output.
    // Must not remove connections or mutate manager
    // registrations during invocation.
    using InputHandler = std::function<void(Connection&)>;

    // Optional transport-independent resumption contract.
    //
    // AwaitRead: wait for another socket read.
    // Continue: schedule another bounded processing turn.
    // AwaitWrite: resume only after output makes progress.
    enum class InputAction { AwaitRead, Continue, AwaitWrite };

    using ResumableInputHandler = std::function<InputAction(Connection&)>;

    ConnectionManager(TcpListener listener, EventPoller& poller);

    ConnectionManager(TcpListener listener, EventPoller& poller, Limits limits);

    ~ConnectionManager() noexcept;

    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    ConnectionManager(ConnectionManager&&) = delete;
    ConnectionManager& operator=(ConnectionManager&&) = delete;

    [[nodiscard]] int listener_fd() const noexcept { return listener_.fd(); }

    [[nodiscard]] std::uint16_t listener_port() const noexcept { return listener_.port(); }

    [[nodiscard]] bool listener_registered() const noexcept { return listener_registered_; }

    [[nodiscard]] RegistrationToken listener_token() const noexcept {
        return listener_registered_ ? listener_token_ : InvalidToken;
    }

    [[nodiscard]] bool is_listener_event(RegistrationToken token) const noexcept {
        return listener_registered_ && token != InvalidToken && token == listener_token_;
    }

    [[nodiscard]] std::size_t connection_count() const noexcept { return connections_.size(); }

    [[nodiscard]] Connection* find(RegistrationToken token) noexcept;

    [[nodiscard]] const Connection* find(RegistrationToken token) const noexcept;

    // Accept up to min(budget, max_accept_per_call).
    //
    // Stops on EAGAIN, budget exhaustion or capacity.
    // Registers each accepted FD with Read interest.
    //
    // An invalid zero budget throws.
    [[nodiscard]] AcceptResult accept_ready(std::size_t budget);

    // Idempotent for absent connection tokens.
    // Unregisters before closing the owned socket.
    //
    // If capacity had been reached, a successful
    // close resumes the listening registration
    // using a fresh, never-reused token.
    [[nodiscard]] bool close_connection(RegistrationToken token);

    void set_input_handler(InputHandler handler);

    // Exclusive with the legacy input handler.
    // Its callback must not mutate manager registrations.
    void set_resumable_input_handler(ResumableInputHandler handler);

    void refresh_interest(RegistrationToken token);

    void stop_accepting();
    [[nodiscard]] bool has_pending_work() const noexcept;

    // True when reading is suspended because of
    // output pressure or received EOF.
    [[nodiscard]] bool read_paused(RegistrationToken token) const noexcept;

    [[nodiscard]] DispatchResult dispatch_ready(const std::vector<ReadyEvent>& events);

    [[nodiscard]] DispatchResult poll_once(std::chrono::milliseconds timeout);

    // Explicit clock input supports deterministic tests.
    // Returns the number of connections evicted.
    [[nodiscard]] std::size_t reap_idle_connections(std::chrono::steady_clock::time_point now);

  private:
    [[nodiscard]] RegistrationToken allocate_token();

    void enqueue_deferred(RegistrationToken token);
    [[nodiscard]] bool run_resumable(RegistrationToken token, DispatchResult& stats);
    [[nodiscard]] DispatchResult run_deferred();

    void suspend_listener();
    void resume_listener();

    TcpListener listener_;
    EventPoller& poller_;
    Limits limits_;

    std::unordered_map<RegistrationToken, std::unique_ptr<Connection>> connections_;

    RegistrationToken listener_token_ = InvalidToken;
    RegistrationToken next_token_ = 1;
    bool listener_registered_ = false;
    bool stopping_ = false;
    InputHandler input_handler_;
    ResumableInputHandler resumable_input_handler_;

    std::unordered_set<RegistrationToken> read_paused_;

    std::deque<RegistrationToken> deferred_tokens_;
    std::unordered_set<RegistrationToken> deferred_pending_;
    std::unordered_set<RegistrationToken> waiting_for_write_;
};

} // namespace redisdb::net
