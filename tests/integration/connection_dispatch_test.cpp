#include "redisdb/net/ConnectionManager.hpp"

#if defined(__APPLE__)
#include "redisdb/net/KqueuePoller.hpp"
using NativePoller = redisdb::net::KqueuePoller;
#elif defined(__linux__)
#include "redisdb/net/EpollPoller.hpp"
using NativePoller = redisdb::net::EpollPoller;
#else
#error "Native dispatch test requires macOS or Linux"
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using redisdb::net::Connection;
using redisdb::net::ConnectionManager;
using redisdb::net::EventMask;
using redisdb::net::ReadyEvent;
using redisdb::net::RegistrationToken;
using redisdb::net::TcpListener;
using redisdb::net::UniqueFd;

using namespace std::chrono_literals;

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] UniqueFd connect_loopback(std::uint16_t port) {
    const int raw = ::socket(AF_INET, SOCK_STREAM, 0);

    check(raw >= 0, "client socket creation failed");

    UniqueFd client(raw);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    check(::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                    static_cast<socklen_t>(sizeof(address))) == 0,
          "TCP connect failed");

    return client;
}

void send_exact(int fd, std::string_view data) {
    check(::send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size()),
          "test send failed");
}

void receive_exact(int fd, std::string_view expected) {
    std::string actual(expected.size(), '\0');

    check(::recv(fd, actual.data(), actual.size(), MSG_WAITALL) ==
              static_cast<ssize_t>(actual.size()),
          "test recv length mismatch");

    check(actual == expected, "test response mismatch");
}

void test_dispatch_round_trip() {
    NativePoller poller;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller);

    std::size_t callbacks = 0;

    manager.set_input_handler([&](Connection& connection) {
        ++callbacks;

        const std::string payload(connection.input());

        connection.consume_input(payload.size());

        // Transport test only, not server behavior.
        connection.queue_output("ACK:" + payload);
    });

    UniqueFd client = connect_loopback(manager.listener_port());

    const auto accepted = manager.poll_once(1000ms);

    check(accepted.accepted == 1, "dispatch did not accept a client");

    check(manager.connection_count() == 1, "accepted client missing");

    send_exact(client.get(), "hi");

    const auto received = manager.poll_once(1000ms);

    check(received.bytes_received == 2, "read dispatch did not consume socket data");

    check(callbacks == 1, "input callback not invoked exactly once");

    std::size_t sent = received.bytes_sent;

    for (int attempt = 0; attempt < 10 && sent < 6; ++attempt) {
        sent += manager.poll_once(1000ms).bytes_sent;
    }

    check(sent == 6, "queued response was not sent");

    receive_exact(client.get(), "ACK:hi");

    check(manager.connection_count() == 1, "healthy client unexpectedly closed");

    client.reset();

    for (int attempt = 0; attempt < 5 && manager.connection_count() != 0; ++attempt) {
        (void)manager.poll_once(1000ms);
    }

    check(manager.connection_count() == 0, "peer disconnect was not cleaned up");
}

void test_external_output_and_stale_events() {
    NativePoller poller;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller);

    UniqueFd client = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "client acceptance failed");

    // Listener starts at token 1; accepted tokens
    // are allocated monotonically from the same source.
    // Discover the live token from readiness by
    // sending a byte rather than depending on its value.
    send_exact(client.get(), "X");

    const auto ready = poller.wait(1000ms);

    RegistrationToken token = redisdb::net::InvalidToken;

    for (const auto& event : ready) {
        if (manager.find(event.token) != nullptr) {
            token = event.token;
            break;
        }
    }

    check(token != redisdb::net::InvalidToken, "connection token not found");

    const auto read_stats = manager.dispatch_ready(ready);

    check(read_stats.bytes_received == 1, "manual event dispatch did not read data");

    Connection* connection = manager.find(token);

    check(connection != nullptr, "connection lookup failed");

    connection->consume_input(1);
    connection->queue_output("DATA");

    manager.refresh_interest(token);

    std::size_t sent = 0;

    for (int attempt = 0; attempt < 10 && sent < 4; ++attempt) {
        sent += manager.poll_once(1000ms).bytes_sent;
    }

    check(sent == 4, "external output not dispatched");

    receive_exact(client.get(), "DATA");

    check(manager.close_connection(token), "explicit close failed");

    const auto stale = manager.dispatch_ready({{token, EventMask::Read}});

    check(stale.stale_events == 1, "removed token was not ignored");

    check(manager.connection_count() == 0, "stale readiness recreated a connection");
}

void test_input_safety_limit() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.connection.max_input_bytes = 3;
    limits.connection.max_output_bytes = 16;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller, limits);

    UniqueFd client = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "capacity test client was not accepted");

    send_exact(client.get(), "abcd");

    const auto result = manager.poll_once(1000ms);

    check(result.bytes_received == 3, "input limit was not enforced");

    check(result.closed == 1, "full input buffer was not closed");

    check(manager.connection_count() == 0, "full-buffer connection remained registered");
}

void test_listener_token_rotation() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.max_connections = 1;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller, limits);

    const RegistrationToken first_listener = manager.listener_token();

    UniqueFd first = connect_loopback(manager.listener_port());

    const auto accepted = manager.poll_once(1000ms);

    check(accepted.accepted == 1, "first client not accepted");

    check(!manager.listener_registered(), "full listener not suspended");

    send_exact(first.get(), "Z");

    const auto client_events = poller.wait(1000ms);

    RegistrationToken first_client = redisdb::net::InvalidToken;

    for (const auto& event : client_events) {
        if (manager.find(event.token)) {
            first_client = event.token;
            break;
        }
    }

    check(first_client != redisdb::net::InvalidToken, "first client token missing");

    check(manager.close_connection(first_client), "first client close failed");

    check(manager.listener_registered(), "listener did not resume");

    const RegistrationToken resumed = manager.listener_token();

    check(resumed != first_listener, "listener token was reused");

    const auto stale = manager.dispatch_ready(
        {{first_listener, EventMask::Read}, {first_client, EventMask::Read}});

    check(stale.stale_events == 2, "stale listener/client tokens not ignored");

    UniqueFd second = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "resumed listener did not accept client");
}

void test_invalid_event_budgets() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.max_read_per_event = 0;

    bool rejected_read = false;

    try {
        ConnectionManager invalid(TcpListener::bind_loopback(0), poller, limits);
    } catch (const std::invalid_argument&) {
        rejected_read = true;
    }

    check(rejected_read, "zero read budget accepted");

    limits.max_read_per_event = 1;
    limits.max_write_per_event = 0;

    bool rejected_write = false;

    try {
        ConnectionManager invalid(TcpListener::bind_loopback(0), poller, limits);
    } catch (const std::invalid_argument&) {
        rejected_write = true;
    }

    check(rejected_write, "zero write budget accepted");
}

void test_duplicate_readiness_notifications() {
    NativePoller poller;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller);

    UniqueFd client = connect_loopback(manager.listener_port());

    const RegistrationToken listener = manager.listener_token();

    // Synchronize TCP readiness before dispatching.
    const auto listener_ready = poller.wait(1000ms);

    check(!listener_ready.empty(), "listener readiness was not observed");

    const auto accepted =
        manager.dispatch_ready({{listener, EventMask::Read}, {listener, EventMask::Read}});

    check(accepted.accepted == 1, "duplicate listener event caused extra work");

    check(accepted.duplicate_events == 1, "duplicate listener event not counted");

    check(manager.connection_count() == 1, "duplicate listener event corrupted count");

    std::size_t callbacks = 0;

    manager.set_input_handler([&](Connection& connection) {
        ++callbacks;
        connection.consume_input(connection.input().size());
    });

    send_exact(client.get(), "Z");

    const auto client_ready = poller.wait(1000ms);

    RegistrationToken client_token = redisdb::net::InvalidToken;

    for (const auto& event : client_ready) {
        if (manager.find(event.token)) {
            client_token = event.token;
            break;
        }
    }

    check(client_token != redisdb::net::InvalidToken, "client readiness was not observed");

    const auto read =
        manager.dispatch_ready({{client_token, EventMask::Read}, {client_token, EventMask::Read}});

    check(read.bytes_received == 1, "duplicate event changed read budget");

    check(read.duplicate_events == 1, "duplicate client event not counted");

    check(callbacks == 1, "input callback ran more than once");
}

void test_deterministic_idle_reaping() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.idle_timeout = 30ms;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller, limits);

    UniqueFd client = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "idle reaper setup failed");

    send_exact(client.get(), "K");

    const auto ready = poller.wait(1000ms);

    RegistrationToken token = redisdb::net::InvalidToken;

    for (const auto& event : ready) {
        if (manager.find(event.token)) {
            token = event.token;
            break;
        }
    }

    check(token != redisdb::net::InvalidToken, "idle connection token missing");

    const Connection* connection = manager.find(token);

    check(connection != nullptr, "idle connection not found");

    const auto last = connection->last_activity();

    check(manager.reap_idle_connections(last + 20ms) == 0, "active connection evicted too early");

    check(manager.connection_count() == 1, "early reaper changed connection count");

    check(manager.reap_idle_connections(last + 31ms) == 1, "expired connection was not evicted");

    check(manager.connection_count() == 0, "expired connection still owned");

    check(manager.find(token) == nullptr, "expired token still resolves");

    check(manager.reap_idle_connections(last + 40ms) == 0, "idle reaping was not idempotent");
}

void test_idle_deadline_bounds_infinite_poll() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.idle_timeout = 30ms;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller, limits);

    UniqueFd client = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "idle deadline setup failed");

    // No client data is sent. An indefinite poll
    // must still wake for the nearest idle deadline.
    const auto start = std::chrono::steady_clock::now();

    const auto result = manager.poll_once(std::chrono::milliseconds{-1});

    const auto elapsed = std::chrono::steady_clock::now() - start;

    check(result.closed == 1, "idle deadline did not evict connection");

    check(manager.connection_count() == 0, "idle deadline left connection open");

    check(elapsed < 750ms, "idle deadline did not bound poll duration");
}

void test_disabled_and_invalid_idle_timeout() {
    {
        NativePoller poller;

        ConnectionManager manager(TcpListener::bind_loopback(0), poller);

        UniqueFd client = connect_loopback(manager.listener_port());

        check(manager.poll_once(1000ms).accepted == 1, "default idle test setup failed");

        const auto future = std::chrono::steady_clock::now() + std::chrono::hours{24};

        check(manager.reap_idle_connections(future) == 0,
              "disabled idle timeout evicted connection");

        check(manager.connection_count() == 1, "disabled timeout changed ownership");
    }

    {
        NativePoller poller;

        ConnectionManager::Limits limits;
        limits.idle_timeout = -1ms;

        bool rejected = false;

        try {
            ConnectionManager invalid(TcpListener::bind_loopback(0), poller, limits);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }

        check(rejected, "negative idle timeout was accepted");
    }
}

void test_split_read_write_events_are_combined() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.max_read_per_event = 1;
    limits.max_write_per_event = 2;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller, limits);

    UniqueFd client = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "split-event client was not accepted");

    send_exact(client.get(), "AB");

    const auto ready = poller.wait(1000ms);

    RegistrationToken token = redisdb::net::InvalidToken;

    for (const auto& event : ready) {
        if (manager.find(event.token) != nullptr) {
            token = event.token;
            break;
        }
    }

    check(token != redisdb::net::InvalidToken, "split-event client token not found");

    Connection* connection = manager.find(token);

    check(connection != nullptr, "split-event connection lookup failed");

    connection->queue_output("WXYZ");
    manager.refresh_interest(token);

    // Mimic native kqueue's separate Read and
    // Write notifications for one registration.
    const auto result =
        manager.dispatch_ready({{token, EventMask::Read}, {token, EventMask::Write}});

    check(result.duplicate_events == 1, "split records were not coalesced");

    check(result.bytes_received == 1, "split-event read budget not enforced");

    check(result.bytes_sent == 2, "split-event write was lost or over budget");

    check(connection->input() == "A", "split-event input was incorrect");

    check(connection->pending_output_bytes() == 2, "split-event pending output incorrect");

    // Only read after dispatch confirms two bytes
    // were actually written to the test socket.
    receive_exact(client.get(), "WX");

    check(manager.close_connection(token), "split-event cleanup failed");
}

void test_coalesced_error_takes_priority() {
    NativePoller poller;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller);

    UniqueFd client = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "error-priority client was not accepted");

    send_exact(client.get(), "E");

    const auto ready = poller.wait(1000ms);

    RegistrationToken token = redisdb::net::InvalidToken;

    for (const auto& event : ready) {
        if (manager.find(event.token) != nullptr) {
            token = event.token;
            break;
        }
    }

    check(token != redisdb::net::InvalidToken, "error-priority token not found");

    const auto result =
        manager.dispatch_ready({{token, EventMask::Read}, {token, EventMask::Error}});

    check(result.duplicate_events == 1, "error record was not coalesced");

    check(result.closed == 1, "coalesced error did not close connection");

    check(result.bytes_received == 0, "error priority allowed unexpected read");

    check(manager.connection_count() == 0, "error-priority connection remained live");

    check(manager.find(token) == nullptr, "closed registration token still resolves");
}

void test_output_watermark_hysteresis() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.connection.max_output_bytes = 16;
    limits.max_write_per_event = 2;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller, limits);

    UniqueFd client = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "watermark client acceptance failed");

    send_exact(client.get(), "I");

    const auto ready = poller.wait(1000ms);

    RegistrationToken token = redisdb::net::InvalidToken;

    for (const auto& event : ready) {
        if (manager.find(event.token) != nullptr) {
            token = event.token;
            break;
        }
    }

    check(token != redisdb::net::InvalidToken, "watermark connection token missing");

    const auto initial = manager.dispatch_ready(ready);

    check(initial.bytes_received == 1, "watermark setup read failed");

    Connection* connection = manager.find(token);

    check(connection != nullptr, "watermark connection lookup failed");

    connection->consume_input(1);

    check(!manager.read_paused(token), "fresh connection incorrectly paused");

    // Maximum output is 16 bytes.
    // High watermark = 12, low watermark = 8.
    connection->queue_output("abcdefghijkl");

    manager.refresh_interest(token);

    check(manager.read_paused(token), "read interest did not pause at high watermark");

    check(connection->pending_output_bytes() == 12, "high watermark pending count incorrect");

    // Data arriving while output pressure is high
    // must not be read until output drains.
    send_exact(client.get(), "R");

    std::size_t total_sent = 0;

    for (int index = 0; index < 2; ++index) {
        const auto step = manager.poll_once(1000ms);

        check(step.bytes_received == 0, "paused client performed an unexpected read");

        check(step.bytes_sent == 2, "bounded write did not make expected progress");

        total_sent += step.bytes_sent;

        if (index == 0) {
            check(manager.read_paused(token), "reads resumed above low watermark");

            check(connection->pending_output_bytes() == 10, "first drain pending count incorrect");
        }
    }

    check(connection->pending_output_bytes() == 8, "low watermark pending count incorrect");

    check(!manager.read_paused(token), "reads did not resume at low watermark");

    bool received_after_resume = false;

    for (int index = 0; index < 20 && (!received_after_resume || total_sent < 12); ++index) {
        const auto step = manager.poll_once(1000ms);

        total_sent += step.bytes_sent;

        if (step.bytes_received == 1) {
            received_after_resume = true;
        }
    }

    check(received_after_resume, "resumed connection did not read buffered data");

    check(connection->input() == "R", "resumed input payload mismatch");

    check(total_sent == 12, "queued output was not fully drained");

    check(connection->pending_output_bytes() == 0, "output remained buffered after draining");

    check(!manager.read_paused(token), "connection remained paused after draining");

    receive_exact(client.get(), "abcdefghijkl");

    check(manager.close_connection(token), "watermark test cleanup failed");

    check(!manager.read_paused(token), "closed connection retained pause state");
}

void test_callback_runtime_failure_isolated() {
    NativePoller poller;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller);

    manager.set_input_handler(
        [](Connection&) { throw std::runtime_error("simulated request handler failure"); });

    UniqueFd first = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "failing handler client not accepted");

    send_exact(first.get(), "X");

    // An application callback exception must not
    // escape the manager's event dispatch loop.
    const auto failure = manager.poll_once(1000ms);

    check(failure.handler_failures == 1, "callback exception was not recorded");

    check(failure.closed == 1, "failed callback did not close client");

    check(failure.output_limit_closes == 0, "runtime failure misclassified as overflow");

    check(manager.connection_count() == 0, "failed callback left connection registered");

    // The listener must remain operational after
    // a single client has failed.
    manager.set_input_handler([](Connection& connection) {
        connection.consume_input(connection.input().size());

        connection.queue_output("OK");
    });

    UniqueFd second = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1,
          "listener did not recover from callback failure");

    send_exact(second.get(), "Y");

    const auto second_read = manager.poll_once(1000ms);

    check(second_read.bytes_received == 1, "healthy follow-up client was not processed");

    std::size_t sent = second_read.bytes_sent;

    for (int index = 0; index < 10 && sent < 2; ++index) {
        sent += manager.poll_once(1000ms).bytes_sent;
    }

    check(sent == 2, "healthy follow-up response was not sent");

    receive_exact(second.get(), "OK");

    check(manager.connection_count() == 1, "healthy follow-up client unexpectedly closed");
}

void test_callback_output_limit_isolated() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.connection.max_output_bytes = 4;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller, limits);

    manager.set_input_handler([](Connection& connection) { connection.queue_output("12345"); });

    UniqueFd first = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "overflow client not accepted");

    send_exact(first.get(), "O");

    const auto overflow = manager.poll_once(1000ms);

    check(overflow.output_limit_closes == 1, "output overflow not recorded");

    check(overflow.handler_failures == 0, "output overflow misclassified");

    check(overflow.closed == 1, "overflowing client was not closed");

    check(manager.connection_count() == 0, "overflowing client remained registered");

    // Confirm that a rejected client's failure
    // does not prevent subsequent normal requests.
    manager.set_input_handler([](Connection& connection) {
        connection.consume_input(connection.input().size());

        connection.queue_output("OK");
    });

    UniqueFd second = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "listener failed after output overflow");

    send_exact(second.get(), "B");

    const auto healthy = manager.poll_once(1000ms);

    check(healthy.bytes_received == 1, "follow-up request after overflow failed");

    std::size_t sent = healthy.bytes_sent;

    for (int index = 0; index < 10 && sent < 2; ++index) {
        sent += manager.poll_once(1000ms).bytes_sent;
    }

    check(sent == 2, "follow-up response after overflow failed");

    receive_exact(second.get(), "OK");
}

void test_half_closed_client_drains_output() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.max_write_per_event = 2;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller, limits);

    std::size_t callbacks = 0;

    manager.set_input_handler([&](Connection& connection) {
        ++callbacks;

        connection.consume_input(connection.input().size());

        connection.queue_output("REPLY");
    });

    UniqueFd client = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "half-close client not accepted");

    send_exact(client.get(), "Q");

    // Stop sending while retaining the receiving
    // half of the client's TCP connection.
    check(::shutdown(client.get(), SHUT_WR) == 0, "client write-half shutdown failed");

    std::size_t total_received = 0;
    std::size_t total_sent = 0;
    std::size_t total_closed = 0;

    for (int index = 0; index < 20 && manager.connection_count() != 0; ++index) {
        const auto result = manager.poll_once(1000ms);

        total_received += result.bytes_received;
        total_sent += result.bytes_sent;
        total_closed += result.closed;
    }

    check(callbacks == 1, "half-closed request callback count incorrect");

    check(total_received == 1, "half-closed request was not received");

    check(total_sent == 5, "half-closed response was not fully sent");

    check(total_closed == 1, "half-closed connection was not cleaned up");

    check(manager.connection_count() == 0, "half-closed connection remained registered");

    // After shutdown(SHUT_WR), the client can still
    // receive all bytes sent by the server.
    receive_exact(client.get(), "REPLY");
}

void test_client_error_notification_isolated() {
    NativePoller poller;
    ConnectionManager manager(TcpListener::bind_loopback(0), poller);

    UniqueFd first = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "error test client not accepted");

    send_exact(first.get(), "X");

    const auto ready = poller.wait(1000ms);
    RegistrationToken token = redisdb::net::InvalidToken;

    for (const auto& event : ready) {
        if (manager.find(event.token)) {
            token = event.token;
            break;
        }
    }

    check(token != redisdb::net::InvalidToken, "error test connection token missing");

    const auto failure = manager.dispatch_ready({{token, EventMask::Error}});

    check(failure.closed == 1, "client error did not close connection");
    check(failure.io_failures == 1, "client error was not counted");
    check(manager.connection_count() == 0, "failed client remained registered");

    manager.set_input_handler([](Connection& connection) {
        connection.consume_input(connection.input().size());
        connection.queue_output("OK");
    });

    UniqueFd second = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "listener failed after client error");

    send_exact(second.get(), "Y");

    std::size_t sent = 0;
    for (int i = 0; i < 10 && sent < 2; ++i) {
        sent += manager.poll_once(1000ms).bytes_sent;
    }

    check(sent == 2, "healthy client response failed");
    receive_exact(second.get(), "OK");
}

void test_listener_error_propagates() {
    NativePoller poller;
    ConnectionManager manager(TcpListener::bind_loopback(0), poller);

    bool rejected = false;

    try {
        (void)manager.dispatch_ready({{manager.listener_token(), EventMask::Error}});
    } catch (const std::runtime_error&) {
        rejected = true;
    }

    check(rejected, "listener error was silently ignored");
    check(manager.listener_registered(), "listener error unexpectedly removed registration");
}

void test_abrupt_client_reset() {
    NativePoller poller;
    ConnectionManager manager(TcpListener::bind_loopback(0), poller);

    UniqueFd client = connect_loopback(manager.listener_port());

    check(manager.poll_once(1000ms).accepted == 1, "reset test client not accepted");

    linger reset_linger{};
    reset_linger.l_onoff = 1;
    reset_linger.l_linger = 0;

    check(::setsockopt(client.get(), SOL_SOCKET, SO_LINGER, &reset_linger,
                       static_cast<socklen_t>(sizeof(reset_linger))) == 0,
          "failed to configure abrupt client reset");

    client.reset();

    for (int i = 0; i < 10 && manager.connection_count() != 0; ++i) {
        (void)manager.poll_once(100ms);
    }

    check(manager.connection_count() == 0, "abruptly reset client remained registered");
}

void test_healthy_client_progress_under_pressure() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.max_connections = 2;
    limits.max_write_per_event = 2;
    limits.connection.max_output_bytes = 64;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller, limits);

    UniqueFd slow = connect_loopback(manager.listener_port());
    UniqueFd fast = connect_loopback(manager.listener_port());

    for (int i = 0; i < 5 && manager.connection_count() < 2; ++i) {
        (void)manager.poll_once(1000ms);
    }

    check(manager.connection_count() == 2, "two-client fairness setup failed");

    send_exact(slow.get(), "S");

    const auto ready = poller.wait(1000ms);
    RegistrationToken slow_token = redisdb::net::InvalidToken;

    for (const auto& event : ready) {
        if (manager.find(event.token)) {
            slow_token = event.token;
            break;
        }
    }

    check(slow_token != redisdb::net::InvalidToken, "slow connection token missing");

    const auto setup = manager.dispatch_ready(ready);
    check(setup.bytes_received == 1, "slow connection setup read failed");

    Connection* slow_connection = manager.find(slow_token);

    check(slow_connection != nullptr, "slow connection lookup failed");

    slow_connection->consume_input(1);
    slow_connection->queue_output(std::string(60, 'S'));
    manager.refresh_interest(slow_token);

    check(manager.read_paused(slow_token), "slow connection did not pause reads");

    bool fast_processed = false;
    bool slow_read_while_paused = false;
    RegistrationToken fast_token = redisdb::net::InvalidToken;

    manager.set_input_handler([&](Connection& connection) {
        const std::string payload(connection.input());
        connection.consume_input(payload.size());

        if (payload == "F") {
            fast_processed = true;
            fast_token = connection.token();
            connection.queue_output("OK");
        }

        if (payload == "Q") {
            slow_read_while_paused = true;
        }
    });

    // Queue more slow-client input while its read
    // interest is disabled.
    send_exact(slow.get(), "Q");
    send_exact(fast.get(), "F");

    bool fast_response_drained = false;

    for (int i = 0; i < 10; ++i) {
        (void)manager.poll_once(1000ms);

        if (fast_processed) {
            const Connection* active = manager.find(fast_token);

            if (active != nullptr && !active->wants_write()) {
                fast_response_drained = true;
                break;
            }
        }
    }

    check(fast_processed, "healthy client request was starved");

    check(fast_response_drained, "healthy client response was starved");

    check(!slow_read_while_paused, "paused slow connection was read");

    check(manager.read_paused(slow_token), "slow connection resumed prematurely");

    check(slow_connection->pending_output_bytes() > 32, "slow output drained beyond test pressure");

    receive_exact(fast.get(), "OK");
}

} // namespace

int main() {
    try {
        test_dispatch_round_trip();
        test_external_output_and_stale_events();
        test_input_safety_limit();
        test_listener_token_rotation();
        test_invalid_event_budgets();
        test_duplicate_readiness_notifications();
        test_deterministic_idle_reaping();
        test_idle_deadline_bounds_infinite_poll();
        test_disabled_and_invalid_idle_timeout();
        test_split_read_write_events_are_combined();
        test_coalesced_error_takes_priority();
        test_output_watermark_hysteresis();
        test_callback_runtime_failure_isolated();
        test_callback_output_limit_isolated();
        test_half_closed_client_drains_output();
        test_client_error_notification_isolated();
        test_listener_error_propagates();
        test_abrupt_client_reset();
        test_healthy_client_progress_under_pressure();

        std::cout << "Connection readiness dispatch tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Connection dispatch test failed: " << error.what() << '\n';

        return 1;
    }
}
