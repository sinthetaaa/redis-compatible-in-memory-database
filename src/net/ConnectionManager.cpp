#include "redisdb/net/ConnectionManager.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace redisdb::net {

ConnectionManager::ConnectionManager(TcpListener listener, EventPoller& poller)
    : ConnectionManager(std::move(listener), poller, Limits{}) {}

ConnectionManager::ConnectionManager(TcpListener listener, EventPoller& poller, Limits limits)
    : listener_(std::move(listener)), poller_(poller), limits_(limits) {
    if (listener_.fd() < 0) {
        throw std::invalid_argument("connection manager requires a valid listener");
    }

    if (limits_.max_connections == 0 || limits_.max_accept_per_call == 0 ||
        limits_.max_read_per_event == 0 || limits_.max_write_per_event == 0 ||
        limits_.connection.max_input_bytes == 0 || limits_.connection.max_output_bytes == 0) {
        throw std::invalid_argument("connection manager limits must be positive");
    }

    const RegistrationToken token = allocate_token();

    poller_.add(listener_.fd(), EventMask::Read, token);

    listener_token_ = token;
    listener_registered_ = true;
}

ConnectionManager::~ConnectionManager() noexcept {
    // Normal close_connection() paths propagate removal
    // errors before releasing descriptor ownership.
    //
    // Destruction cannot throw. Unregister every live
    // connection before its UniqueFd is destroyed.
    for (const auto& [token, connection] : connections_) {
        (void)token;

        try {
            poller_.remove(connection->fd());
        } catch (...) {
            // Best-effort destructor cleanup only.
        }
    }

    if (listener_registered_) {
        try {
            poller_.remove(listener_.fd());
        } catch (...) {
            // Best-effort destructor cleanup only.
        }
    }
}

RegistrationToken ConnectionManager::allocate_token() {
    constexpr RegistrationToken MaxToken = std::numeric_limits<RegistrationToken>::max();

    if (next_token_ == InvalidToken || next_token_ == MaxToken) {
        throw std::overflow_error("registration token space exhausted");
    }

    const RegistrationToken result = next_token_;
    ++next_token_;

    return result;
}

Connection* ConnectionManager::find(RegistrationToken token) noexcept {
    const auto it = connections_.find(token);

    return it != connections_.end() ? it->second.get() : nullptr;
}

const Connection* ConnectionManager::find(RegistrationToken token) const noexcept {
    const auto it = connections_.find(token);

    return it != connections_.end() ? it->second.get() : nullptr;
}

void ConnectionManager::suspend_listener() {
    if (!listener_registered_) {
        return;
    }

    poller_.remove(listener_.fd());

    listener_registered_ = false;
}

void ConnectionManager::resume_listener() {
    if (listener_registered_ || connections_.size() >= limits_.max_connections) {
        return;
    }

    // Never reuse a previous listener token.
    const RegistrationToken token = allocate_token();

    poller_.add(listener_.fd(), EventMask::Read, token);

    listener_token_ = token;
    listener_registered_ = true;
}

ConnectionManager::AcceptResult ConnectionManager::accept_ready(std::size_t budget) {
    if (budget == 0) {
        throw std::invalid_argument("accept budget must be positive");
    }

    AcceptResult result;

    if (connections_.size() >= limits_.max_connections) {
        suspend_listener();
        result.capacity_reached = true;
        return result;
    }

    // In Phase 1F-B the listener can only be disabled
    // when capacity has been reached. Re-enabling is
    // handled by close_connection().
    if (!listener_registered_) {
        throw std::logic_error("listener registration is suspended");
    }

    const std::size_t remaining_capacity = limits_.max_connections - connections_.size();

    const std::size_t maximum = std::min({budget, limits_.max_accept_per_call, remaining_capacity});

    // Allocate result storage before accepting sockets.
    result.tokens.reserve(maximum);

    for (std::size_t index = 0; index < maximum; ++index) {
        auto accepted = listener_.accept_one();

        if (!accepted.has_value()) {
            result.would_block = true;
            break;
        }

        // Accept returns a non-blocking owning UniqueFd.
        const RegistrationToken token = allocate_token();

        auto connection =
            std::make_unique<Connection>(std::move(*accepted), token, limits_.connection);

        const int fd = connection->fd();

        const auto [it, inserted] = connections_.emplace(token, std::move(connection));

        if (!inserted) {
            throw std::logic_error("duplicate connection token");
        }

        try {
            poller_.add(fd, EventMask::Read, token);
        } catch (...) {
            // Poller backends roll back failed ADD.
            // Destroying the entry releases the socket.
            connections_.erase(it);
            throw;
        }

        result.tokens.push_back(token);
    }

    if (connections_.size() >= limits_.max_connections) {
        suspend_listener();
        result.capacity_reached = true;
    }

    return result;
}

bool ConnectionManager::close_connection(RegistrationToken token) {
    const auto it = connections_.find(token);

    if (it == connections_.end()) {
        return false;
    }

    // This operation must precede socket destruction
    // to avoid applying registrations to reused FDs.
    poller_.remove(it->second->fd());

    connections_.erase(it);

    // If the listener was suspended at capacity,
    // resume it with a fresh registration token.
    if (!listener_registered_) {
        resume_listener();
    }

    return true;
}

void ConnectionManager::set_input_handler(InputHandler handler) {
    input_handler_ = std::move(handler);
}

void ConnectionManager::refresh_interest(RegistrationToken token) {
    Connection* connection = find(token);

    if (connection == nullptr) {
        throw std::logic_error("unknown connection registration token");
    }

    if (connection->received_eof() && !connection->wants_write()) {
        throw std::logic_error("EOF connection has no pending output");
    }

    EventMask interest = connection->received_eof() ? EventMask::Write : EventMask::Read;

    if (connection->wants_write()) {
        interest |= EventMask::Write;
    }

    poller_.modify(connection->fd(), interest);
}

ConnectionManager::DispatchResult
ConnectionManager::dispatch_ready(const std::vector<ReadyEvent>& events) {
    DispatchResult stats;

    for (const ReadyEvent& event : events) {
        if (is_listener_event(event.token)) {
            if (contains_all(event.events, EventMask::Read)) {
                const auto accepted = accept_ready(limits_.max_accept_per_call);

                stats.accepted += accepted.tokens.size();
            }

            continue;
        }

        Connection* connection = find(event.token);

        if (connection == nullptr) {
            ++stats.stale_events;
            continue;
        }

        if (contains_all(event.events, EventMask::Error)) {
            if (close_connection(event.token)) {
                ++stats.closed;
            }

            continue;
        }

        const bool readable = contains_all(event.events, EventMask::Read) ||
                              contains_all(event.events, EventMask::Hangup);

        if (readable && !connection->received_eof()) {
            const auto received = connection->receive_once(limits_.max_read_per_event);

            stats.bytes_received += received.bytes_read;

            if (received.bytes_read > 0 && input_handler_) {
                try {
                    input_handler_(*connection);
                } catch (...) {
                    (void)close_connection(event.token);
                    throw;
                }
            }

            // Temporary hard input limit.
            // Detailed backpressure follows in 1F-D.
            if (connection->input().size() >= limits_.connection.max_input_bytes) {
                if (close_connection(event.token)) {
                    ++stats.closed;
                }

                continue;
            }
        }

        if (contains_all(event.events, EventMask::Write) && connection->wants_write()) {
            const auto written = connection->flush_once(limits_.max_write_per_event);

            stats.bytes_sent += written.bytes_written;

            if (written.peer_disconnected) {
                if (close_connection(event.token)) {
                    ++stats.closed;
                }

                continue;
            }
        }

        if (connection->received_eof() && !connection->wants_write()) {
            if (close_connection(event.token)) {
                ++stats.closed;
            }

            continue;
        }

        refresh_interest(event.token);
    }

    return stats;
}

ConnectionManager::DispatchResult ConnectionManager::poll_once(std::chrono::milliseconds timeout) {
    return dispatch_ready(poller_.wait(timeout));
}

} // namespace redisdb::net
