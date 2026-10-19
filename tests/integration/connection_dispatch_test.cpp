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

} // namespace

int main() {
    try {
        test_dispatch_round_trip();
        test_external_output_and_stale_events();
        test_input_safety_limit();
        test_listener_token_rotation();
        test_invalid_event_budgets();

        std::cout << "Connection readiness dispatch tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Connection dispatch test failed: " << error.what() << '\n';

        return 1;
    }
}
