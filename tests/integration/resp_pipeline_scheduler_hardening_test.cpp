#include "redisdb/net/ConnectionManager.hpp"
#include "redisdb/protocol/RespFramedInput.hpp"

#if defined(__APPLE__)
#include "redisdb/net/KqueuePoller.hpp"
using NativePoller = redisdb::net::KqueuePoller;
#elif defined(__linux__)
#include "redisdb/net/EpollPoller.hpp"
using NativePoller = redisdb::net::EpollPoller;
#else
#error "Native scheduler tests require macOS or Linux"
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using redisdb::net::Connection;
using redisdb::net::ConnectionManager;
using redisdb::net::EventMask;
using redisdb::net::ReadyEvent;
using redisdb::net::RegistrationToken;
using redisdb::net::TcpListener;
using redisdb::net::UniqueFd;

using redisdb::protocol::ParseStatus;
using redisdb::protocol::RespFramedInput;

using Action = ConnectionManager::InputAction;

using namespace std::chrono_literals;

namespace {

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

UniqueFd connect_loopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    check(fd >= 0, "client socket failed");

    UniqueFd client(fd);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    check(::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                    static_cast<socklen_t>(sizeof(address))) == 0,
          "client connect failed");

    return client;
}

void send_exact(int fd, std::string_view data) {
    check(::send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size()),
          "test send failed");
}

// Wait until the server socket actually contains
// the entire test request before injecting Read.
//
// MSG_PEEK does not consume any bytes.
// The accepted server socket is nonblocking.
void wait_for_server_input(ConnectionManager& manager, RegistrationToken token,
                           std::size_t expected_bytes) {
    const Connection* connection = manager.find(token);

    check(connection != nullptr, "readiness check requires a live connection");

    check(expected_bytes > 0 && expected_bytes <= 64, "invalid readiness byte count");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (std::chrono::steady_clock::now() < deadline) {
        pollfd watched{};
        watched.fd = connection->fd();
        watched.events = POLLIN;

        const int ready = ::poll(&watched, 1, 20);

        if (ready < 0 && errno == EINTR) {
            continue;
        }

        check(ready >= 0, "readiness poll failed");

        if (ready == 0) {
            continue;
        }

        check((watched.revents & (POLLERR | POLLNVAL)) == 0, "unexpected server socket error");

        if ((watched.revents & POLLIN) == 0) {
            continue;
        }

        char preview[64]{};

        const auto available = ::recv(connection->fd(), preview, sizeof(preview), MSG_PEEK);

        if (available >= static_cast<ssize_t>(expected_bytes)) {
            return;
        }

        if (available == 0) {
            throw std::runtime_error("peer closed before request was available");
        }

        if (available < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            throw std::runtime_error("non-consuming readiness check failed");
        }
    }

    throw std::runtime_error("complete TCP test request was not readable");
}

struct Fixture {
    NativePoller poller;
    ConnectionManager manager;

    explicit Fixture(ConnectionManager::Limits limits = {})
        : poller(), manager(TcpListener::bind_loopback(0), poller, limits) {}

    std::vector<RegistrationToken> accept(std::size_t count) {
        check(count > 0, "zero test accept count");

        std::vector<RegistrationToken> accepted;
        accepted.reserve(count);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

        while (accepted.size() < count) {
            // Nonblocking accept may return fewer than
            // count even after connect() succeeds.
            const auto batch = manager.accept_ready(count - accepted.size());

            accepted.insert(accepted.end(), batch.tokens.begin(), batch.tokens.end());

            if (accepted.size() == count) {
                break;
            }

            check(std::chrono::steady_clock::now() < deadline, "timed out accepting test clients");

            // Wait for real listener readiness rather
            // than manually assuming TCP delivery.
            pollfd watched{};
            watched.fd = manager.listener_fd();
            watched.events = POLLIN;

            int ready = -1;

            do {
                ready = ::poll(&watched, 1, 20);
            } while (ready < 0 && errno == EINTR && std::chrono::steady_clock::now() < deadline);

            check(ready >= 0, "listener readiness poll failed");

            if (ready > 0) {
                check((watched.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0,
                      "unexpected listener error");
            }

            // Timeouts and harmless readiness changes
            // simply retry bounded accept.
        }

        check(accepted.size() == count, "expected clients not accepted");

        return accepted;
    }
};

void test_no_progress_continuation_isolated() {
    Fixture fixture;

    auto bad = connect_loopback(fixture.manager.listener_port());

    auto healthy = connect_loopback(fixture.manager.listener_port());

    const auto tokens = fixture.accept(2);

    fixture.manager.set_resumable_input_handler([&](Connection& connection) {
        if (connection.token() == tokens[0]) {
            // No input consumption, no output.
            return Action::Continue;
        }

        const auto count = connection.input().size();

        connection.consume_input(count);
        connection.queue_output(":7\r\n");

        return Action::AwaitRead;
    });

    send_exact(bad.get(), ":1\r\n");
    send_exact(healthy.get(), ":7\r\n");

    wait_for_server_input(fixture.manager, tokens[0], 4);
    wait_for_server_input(fixture.manager, tokens[1], 4);
    const auto result = fixture.manager.dispatch_ready(
        {{tokens[0], EventMask::Read}, {tokens[1], EventMask::Read}});

    check(result.handler_failures == 1 && result.closed == 1,
          "non-progressing client not isolated");

    check(fixture.manager.find(tokens[0]) == nullptr, "non-progressing client still active");

    const auto* remaining = fixture.manager.find(tokens[1]);

    check(remaining != nullptr && remaining->input().empty() &&
              remaining->pending_output_bytes() == 4,
          "healthy peer affected by stalled callback");
}

void test_closed_deferred_token_not_blocking_peer() {
    ConnectionManager::Limits limits;
    limits.max_deferred_per_poll = 1;

    Fixture fixture(limits);

    auto first = connect_loopback(fixture.manager.listener_port());

    auto second = connect_loopback(fixture.manager.listener_port());

    const auto tokens = fixture.accept(2);

    std::unordered_map<RegistrationToken, std::size_t> invocations;

    fixture.manager.set_resumable_input_handler([&](Connection& connection) {
        auto& calls = invocations[connection.token()];
        ++calls;

        if (calls == 1) {
            connection.consume_input(connection.input().size());

            return Action::Continue;
        }

        return Action::AwaitRead;
    });

    send_exact(first.get(), ":1\r\n");
    send_exact(second.get(), ":2\r\n");

    wait_for_server_input(fixture.manager, tokens[0], 4);
    wait_for_server_input(fixture.manager, tokens[1], 4);
    (void)fixture.manager.dispatch_ready(
        {{tokens[0], EventMask::Read}, {tokens[1], EventMask::Read}});

    check(invocations[tokens[0]] == 1 && invocations[tokens[1]] == 1,
          "initial clients not scheduled");

    check(fixture.manager.close_connection(tokens[0]), "first client did not close");

    // With a one-client deferred budget, a stale
    // token must not delay the healthy continuation.
    (void)fixture.manager.poll_once(0ms);

    check(invocations[tokens[1]] == 2, "stale token consumed healthy client's turn");

    check(fixture.manager.connection_count() == 1, "stale token corrupted active connection");
}

void test_fair_round_robin_deferred_budget() {
    ConnectionManager::Limits limits;
    limits.max_deferred_per_poll = 1;

    Fixture fixture(limits);
    RespFramedInput framed;

    auto first = connect_loopback(fixture.manager.listener_port());

    auto second = connect_loopback(fixture.manager.listener_port());

    const auto tokens = fixture.accept(2);

    std::unordered_map<RegistrationToken, std::size_t> processed;

    fixture.manager.set_resumable_input_handler([&](Connection& connection) {
        const auto parsed = framed.poll(connection);

        check(parsed.status == ParseStatus::Complete, "deferred frame did not parse");

        ++processed[connection.token()];

        connection.queue_output(":0\r\n");

        return connection.input().empty() ? Action::AwaitRead : Action::Continue;
    });

    send_exact(first.get(), ":1\r\n:2\r\n:3\r\n");

    send_exact(second.get(), ":4\r\n:5\r\n:6\r\n");

    wait_for_server_input(fixture.manager, tokens[0], 12);
    wait_for_server_input(fixture.manager, tokens[1], 12);
    (void)fixture.manager.dispatch_ready(
        {{tokens[0], EventMask::Read}, {tokens[1], EventMask::Read}});

    check(processed[tokens[0]] == 1 && processed[tokens[1]] == 1,
          "initial fairness callbacks incorrect");

    (void)fixture.manager.poll_once(0ms);

    check(processed[tokens[0]] == 2 && processed[tokens[1]] == 1, "first deferred turn incorrect");

    (void)fixture.manager.poll_once(0ms);

    check(processed[tokens[0]] == 2 && processed[tokens[1]] == 2, "second deferred turn incorrect");

    (void)fixture.manager.poll_once(0ms);

    check(processed[tokens[0]] == 3 && processed[tokens[1]] == 2, "third deferred turn incorrect");

    (void)fixture.manager.poll_once(0ms);

    check(processed[tokens[0]] == 3 && processed[tokens[1]] == 3, "fourth deferred turn incorrect");

    check(fixture.manager.connection_count() == 2, "fair scheduling closed healthy clients");
}

void test_full_buffer_without_continuation_closes() {
    ConnectionManager::Limits limits;

    limits.connection.max_input_bytes = 4;
    limits.connection.max_output_bytes = 64;

    Fixture fixture(limits);

    auto client = connect_loopback(fixture.manager.listener_port());

    const auto tokens = fixture.accept(1);

    std::size_t calls = 0;

    fixture.manager.set_resumable_input_handler([&](Connection& connection) {
        ++calls;

        if (calls == 1) {
            // This is genuine output progress,
            // but does not consume the full input.
            connection.queue_output(":0\r\n");

            return Action::Continue;
        }

        // No more work is scheduled despite a
        // completely full input buffer.
        return Action::AwaitRead;
    });

    send_exact(client.get(), ":1\r\n");

    wait_for_server_input(fixture.manager, tokens[0], 4);
    (void)fixture.manager.dispatch_ready({{tokens[0], EventMask::Read}});

    check(calls == 1, "initial full-buffer callback did not run");

    check(fixture.manager.connection_count() == 1,
          "initial full-buffer client closed unexpectedly");

    const auto second = fixture.manager.poll_once(0ms);

    check(calls == 2 && second.closed == 1, "deferred full-buffer stall was not closed");

    check(fixture.manager.connection_count() == 0, "full-buffer connection remained registered");
}

void test_idle_preserves_deferred_work() {
    ConnectionManager::Limits limits;
    limits.idle_timeout = 30ms;
    Fixture fixture(limits);
    auto client = connect_loopback(fixture.manager.listener_port());
    const auto tokens = fixture.accept(1);
    fixture.manager.set_resumable_input_handler([](Connection& connection) {
        connection.consume_input(1);
        return Action::Continue;
    });
    send_exact(client.get(), "AB");
    wait_for_server_input(fixture.manager, tokens[0], 2);
    (void)fixture.manager.dispatch_ready({{tokens[0], EventMask::Read}});
    const auto last = fixture.manager.find(tokens[0])->last_activity();
    check(fixture.manager.reap_idle_connections(last + 100ms) == 0,
          "idle reaper closed deferred work without pending output");
    check(fixture.manager.has_pending_work(), "deferred work missing at shutdown");
    fixture.manager.stop_accepting();
    check(!fixture.manager.listener_registered(), "shutdown listener still registered");
    check(fixture.manager.close_connection(tokens[0]), "shutdown client not closed");
    check(!fixture.manager.listener_registered(), "shutdown close reopened listener");
    check(!fixture.manager.has_pending_work(), "closed client left pending work");
}

void test_invalid_write_continuation_isolated() {
    Fixture fixture;

    auto client = connect_loopback(fixture.manager.listener_port());

    const auto tokens = fixture.accept(1);

    fixture.manager.set_resumable_input_handler([](Connection&) {
        // Cannot wait for write readiness if no
        // output was ever queued.
        return Action::AwaitWrite;
    });

    send_exact(client.get(), ":1\r\n");

    wait_for_server_input(fixture.manager, tokens[0], 4);
    const auto result = fixture.manager.dispatch_ready({{tokens[0], EventMask::Read}});

    check(result.closed == 1 && result.handler_failures == 1,
          "write continuation without output accepted");

    check(fixture.manager.connection_count() == 0, "invalid continuation client not removed");
}

} // namespace

int main() {
    try {
        test_no_progress_continuation_isolated();
        test_closed_deferred_token_not_blocking_peer();
        test_fair_round_robin_deferred_budget();
        test_full_buffer_without_continuation_closes();
        test_invalid_write_continuation_isolated();
        test_idle_preserves_deferred_work();

        std::cout << "Scheduler hardening tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scheduler hardening failed: " << error.what() << '\n';

        return 1;
    }
}
