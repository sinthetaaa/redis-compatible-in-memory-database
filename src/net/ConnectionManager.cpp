#include "redisdb/net/ConnectionManager.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
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
        limits_.max_deferred_per_poll == 0 || limits_.idle_timeout.count() < 0 ||
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

        std::unique_ptr<Connection> connection;

        try {
            connection =
                std::make_unique<Connection>(std::move(*accepted), token, limits_.connection);
        } catch (const Connection::DisconnectedDuringSetup&) {
            // The client reset before setup finished.
            // RAII releases the accepted socket.
            //
            // Skip only this client, retain the
            // bounded accept budget, and continue
            // serving all healthy connections.
            continue;
        }

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
    read_paused_.erase(token);
    deferred_pending_.erase(token);
    waiting_for_write_.erase(token);

    // Remove closed clients from the scheduler queue,
    // not just its membership set. Otherwise repeated
    // connection churn can accumulate stale tokens.
    std::erase(deferred_tokens_, token);

    // If the listener was suspended at capacity,
    // resume it with a fresh registration token.
    if (!listener_registered_) {
        resume_listener();
    }

    return true;
}

void ConnectionManager::set_input_handler(InputHandler handler) {
    if (handler && resumable_input_handler_) {
        throw std::logic_error("legacy and resumable input handlers are exclusive");
    }

    input_handler_ = std::move(handler);
}

void ConnectionManager::set_resumable_input_handler(ResumableInputHandler handler) {
    if (handler && input_handler_) {
        throw std::logic_error("legacy and resumable input handlers are exclusive");
    }

    resumable_input_handler_ = std::move(handler);

    if (!resumable_input_handler_) {
        deferred_tokens_.clear();
        deferred_pending_.clear();
        waiting_for_write_.clear();
    }
}

void ConnectionManager::enqueue_deferred(RegistrationToken token) {
    if (deferred_pending_.insert(token).second) {
        deferred_tokens_.push_back(token);
    }
}

bool ConnectionManager::run_resumable(RegistrationToken token, DispatchResult& stats) {
    Connection* connection = find(token);

    if (!connection || !resumable_input_handler_) {
        return connection != nullptr;
    }

    try {
        const std::size_t input_before = connection->input().size();

        const std::size_t output_before = connection->pending_output_bytes();

        const InputAction action = resumable_input_handler_(*connection);

        if (action == InputAction::AwaitRead) {
            deferred_pending_.erase(token);
            waiting_for_write_.erase(token);
        } else if (action == InputAction::Continue) {
            // Continue is a request for immediate future
            // CPU time. It must make observable progress
            // or a broken handler can monopolize the loop.
            const bool consumed_input = connection->input().size() < input_before;

            const bool queued_output = connection->pending_output_bytes() > output_before;

            if (!consumed_input && !queued_output) {
                throw std::logic_error("resumable input handler made no progress");
            }

            waiting_for_write_.erase(token);
            enqueue_deferred(token);
        } else if (action == InputAction::AwaitWrite) {
            if (!connection->wants_write()) {
                throw std::logic_error("write continuation without queued output");
            }

            deferred_pending_.erase(token);
            waiting_for_write_.insert(token);
        } else {
            throw std::logic_error("unknown input action");
        }
    } catch (const std::length_error&) {
        if (close_connection(token)) {
            ++stats.closed;
        }

        ++stats.output_limit_closes;
        return false;
    } catch (...) {
        if (close_connection(token)) {
            ++stats.closed;
        }

        ++stats.handler_failures;
        return false;
    }

    return true;
}

ConnectionManager::DispatchResult ConnectionManager::run_deferred() {
    DispatchResult stats;

    // Snapshot the queue length: clients requeued during
    // this pass cannot run again in the same deferred pass.
    const std::size_t budget = std::min(deferred_tokens_.size(), limits_.max_deferred_per_poll);

    for (std::size_t index = 0; index < budget; ++index) {
        const RegistrationToken token = deferred_tokens_.front();

        deferred_tokens_.pop_front();

        if (deferred_pending_.erase(token) == 0) {
            continue;
        }

        if (!run_resumable(token, stats)) {
            continue;
        }

        Connection* connection = find(token);

        if (!connection) {
            continue;
        }

        // A full receive buffer cannot accept another
        // socket read. If no continuation is scheduled,
        // the callback cannot release any space.
        if (connection->input().size() >= limits_.connection.max_input_bytes &&
            !deferred_pending_.contains(token) && !waiting_for_write_.contains(token) &&
            !connection->closing_after_flush()) {
            if (close_connection(token)) {
                ++stats.closed;
            }

            continue;
        }

        if ((connection->received_eof() || connection->closing_after_flush()) &&
            !connection->wants_write()) {
            if (close_connection(token)) {
                ++stats.closed;
            }

            continue;
        }

        refresh_interest(token);
    }

    return stats;
}

bool ConnectionManager::read_paused(RegistrationToken token) const noexcept {
    return read_paused_.contains(token);
}

void ConnectionManager::refresh_interest(RegistrationToken token) {
    Connection* connection = find(token);

    if (connection == nullptr) {
        throw std::logic_error("unknown connection registration token");
    }

    const std::size_t pending = connection->pending_output_bytes();

    if ((connection->received_eof() || connection->closing_after_flush()) && pending == 0) {
        throw std::logic_error("closing connection has no pending output");
    }

    const std::size_t maximum = limits_.connection.max_output_bytes;

    // Pause at 75% and resume at 50%.
    //
    // Subtraction avoids overflow when maximum
    // is near SIZE_MAX.
    const std::size_t high_water = maximum - maximum / 4;

    const std::size_t low_water = maximum / 2;

    const bool previously_paused = read_paused_.contains(token);

    const bool waiting_output = waiting_for_write_.contains(token);

    const bool full_with_deferred =
        connection->input().size() >= limits_.connection.max_input_bytes &&
        deferred_pending_.contains(token);

    const bool should_pause = connection->received_eof() || connection->closing_after_flush() ||
                              waiting_output || full_with_deferred ||
                              (previously_paused ? pending > low_water : pending >= high_water);

    EventMask interest = EventMask::None;

    if (!should_pause) {
        interest |= EventMask::Read;
    }

    if (pending > 0) {
        interest |= EventMask::Write;
    }

    if (!valid_interest(interest)) {
        throw std::logic_error("backpressure produced invalid interest");
    }

    // Allocate tracking state before modifying
    // the native poller. If modify() throws,
    // restore the previous tracking state.
    if (should_pause && !previously_paused) {
        read_paused_.insert(token);
    }

    try {
        poller_.modify(connection->fd(), interest);
    } catch (...) {
        if (should_pause && !previously_paused) {
            read_paused_.erase(token);
        }

        throw;
    }

    if (!should_pause) {
        read_paused_.erase(token);
    }
}

ConnectionManager::DispatchResult
ConnectionManager::dispatch_ready(const std::vector<ReadyEvent>& events) {
    DispatchResult stats;

    // kqueue may report separate Read and Write
    // events for the same registration. Merge their
    // flags before dispatching each token once.
    //
    // Preserve first-seen token order and enforce
    // one read/write budget per readiness batch.
    std::unordered_map<RegistrationToken, std::size_t> positions;

    std::vector<ReadyEvent> combined;

    positions.reserve(events.size());
    combined.reserve(events.size());

    for (const ReadyEvent& event : events) {
        const auto [it, inserted] = positions.try_emplace(event.token, combined.size());

        if (inserted) {
            combined.push_back(event);
        } else {
            combined[it->second].events |= event.events;
            ++stats.duplicate_events;
        }
    }

    for (const ReadyEvent& event : combined) {
        if (is_listener_event(event.token)) {
            if (contains_all(event.events, EventMask::Error) ||
                contains_all(event.events, EventMask::Hangup)) {
                throw std::runtime_error("listener reported error or hangup");
            }

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

            ++stats.io_failures;
            continue;
        }

        const bool readable = contains_all(event.events, EventMask::Read) ||
                              contains_all(event.events, EventMask::Hangup);

        if (readable && !connection->received_eof() && !read_paused_.contains(event.token)) {
            Connection::ReadResult received{};

            try {
                received = connection->receive_once(limits_.max_read_per_event);
            } catch (const std::system_error&) {
                if (close_connection(event.token)) {
                    ++stats.closed;
                }

                ++stats.io_failures;
                continue;
            }

            stats.bytes_received += received.bytes_read;

            if (received.bytes_read > 0) {
                if (resumable_input_handler_) {
                    if (!run_resumable(event.token, stats)) {
                        continue;
                    }
                } else if (input_handler_) {
                    try {
                        input_handler_(*connection);
                    } catch (const std::length_error&) {
                        if (close_connection(event.token)) {
                            ++stats.closed;
                        }

                        ++stats.output_limit_closes;
                        continue;
                    } catch (...) {
                        if (close_connection(event.token)) {
                            ++stats.closed;
                        }

                        ++stats.handler_failures;
                        continue;
                    }
                }
            }

            // EOF may arrive after a previously completed
            // read. Give otherwise unscheduled buffered
            // input one final bounded processing turn.
            if (received.peer_closed && resumable_input_handler_ && !connection->input().empty() &&
                !deferred_pending_.contains(event.token) &&
                !waiting_for_write_.contains(event.token)) {
                if (!run_resumable(event.token, stats)) {
                    continue;
                }
            }

            // Temporary hard input limit.
            // Detailed backpressure follows in 1F-D.
            const bool full = connection->input().size() >= limits_.connection.max_input_bytes;

            const bool can_resume =
                resumable_input_handler_ && (deferred_pending_.contains(event.token) ||
                                             waiting_for_write_.contains(event.token));

            if (full && !can_resume && !connection->closing_after_flush()) {
                if (close_connection(event.token)) {
                    ++stats.closed;
                }

                continue;
            }
        }

        if (contains_all(event.events, EventMask::Write) && connection->wants_write()) {
            Connection::WriteResult written{};

            try {
                written = connection->flush_once(limits_.max_write_per_event);
            } catch (const std::runtime_error&) {
                if (close_connection(event.token)) {
                    ++stats.closed;
                }

                ++stats.io_failures;
                continue;
            }

            stats.bytes_sent += written.bytes_written;

            if (written.bytes_written > 0 && waiting_for_write_.erase(event.token) > 0) {
                enqueue_deferred(event.token);
            }

            if (written.peer_disconnected) {
                if (close_connection(event.token)) {
                    ++stats.closed;
                }

                continue;
            }
        }

        if ((connection->received_eof() || connection->closing_after_flush()) &&
            !connection->wants_write()) {
            // An EOF client may have complete requests
            // deferred by the fairness budget or output
            // backpressure. Process before final close.
            if (resumable_input_handler_ && deferred_pending_.erase(event.token) > 0) {
                if (!run_resumable(event.token, stats)) {
                    continue;
                }
            }

            if (!connection->wants_write()) {
                if (close_connection(event.token)) {
                    ++stats.closed;
                }

                continue;
            }
        }

        refresh_interest(event.token);
    }

    return stats;
}

std::size_t ConnectionManager::reap_idle_connections(std::chrono::steady_clock::time_point now) {
    if (limits_.idle_timeout.count() == 0) {
        return 0;
    }

    std::vector<RegistrationToken> expired;

    for (const auto& [token, connection] : connections_) {
        const auto last = connection->last_activity();

        if (now >= last && now - last >= limits_.idle_timeout) {
            expired.push_back(token);
        }
    }

    std::size_t closed = 0;

    for (RegistrationToken token : expired) {
        if (close_connection(token)) {
            ++closed;
        }
    }

    return closed;
}

ConnectionManager::DispatchResult ConnectionManager::poll_once(std::chrono::milliseconds timeout) {
    if (!valid_wait_timeout(timeout)) {
        throw std::invalid_argument("invalid connection manager wait timeout");
    }

    auto effective_timeout = timeout;

    // An indefinite poll must not prevent the manager
    // from enforcing a configured idle deadline.
    if (limits_.idle_timeout.count() > 0) {
        const auto now = std::chrono::steady_clock::now();

        for (const auto& [token, connection] : connections_) {
            (void)token;

            const auto deadline = connection->last_activity() + limits_.idle_timeout;

            if (deadline <= now) {
                effective_timeout = std::chrono::milliseconds{0};
                break;
            }

            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);

            if (effective_timeout == std::chrono::milliseconds{-1} ||
                remaining < effective_timeout) {
                effective_timeout = remaining;
            }
        }
    }

    const bool had_deferred = !deferred_tokens_.empty();

    auto deferred_stats = run_deferred();

    // Deferred work must not block on new network input.
    if (had_deferred) {
        effective_timeout = std::chrono::milliseconds{0};
    }

    auto stats = dispatch_ready(poller_.wait(effective_timeout));

    stats.closed += deferred_stats.closed;
    stats.handler_failures += deferred_stats.handler_failures;
    stats.output_limit_closes += deferred_stats.output_limit_closes;

    stats.closed += reap_idle_connections(std::chrono::steady_clock::now());

    return stats;
}

} // namespace redisdb::net
