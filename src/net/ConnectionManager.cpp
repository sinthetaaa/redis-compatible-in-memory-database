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

} // namespace redisdb::net
