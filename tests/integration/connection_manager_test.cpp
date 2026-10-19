#include "redisdb/net/ConnectionManager.hpp"

#if defined(__APPLE__)
#include "redisdb/net/KqueuePoller.hpp"
using NativePoller = redisdb::net::KqueuePoller;
#elif defined(__linux__)
#include "redisdb/net/EpollPoller.hpp"
using NativePoller = redisdb::net::EpollPoller;
#else
#error "ConnectionManager native test requires macOS or Linux"
#endif

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using redisdb::net::ConnectionManager;
using redisdb::net::EventMask;
using redisdb::net::ReadyEvent;
using redisdb::net::RegistrationToken;
using redisdb::net::TcpListener;
using redisdb::net::UniqueFd;

using namespace std::chrono_literals;

static_assert(!std::is_copy_constructible_v<ConnectionManager>);

static_assert(!std::is_move_constructible_v<ConnectionManager>);

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Error, typename Action>
void expect_exception(Action&& action, const char* message) {
    bool caught = false;

    try {
        std::forward<Action>(action)();
    } catch (const Error&) {
        caught = true;
    }

    check(caught, message);
}

[[nodiscard]] bool has_event(const std::vector<ReadyEvent>& events, RegistrationToken token,
                             EventMask mask) {
    for (const auto& event : events) {
        if (event.token == token && redisdb::net::contains_all(event.events, mask)) {
            return true;
        }
    }

    return false;
}

[[nodiscard]] UniqueFd connect_loopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    check(fd >= 0, "client socket creation failed");

    UniqueFd socket(fd);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    check(::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address),
                    static_cast<socklen_t>(sizeof(address))) == 0,
          "client connect failed");

    return socket;
}

void send_data(int fd, std::string_view data) {
    check(::send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size()),
          "client send failed");
}

void receive_data(int fd, std::string_view expected) {
    char buffer[32]{};

    check(expected.size() <= sizeof(buffer), "expected test payload too large");

    check(::recv(fd, buffer, expected.size(), 0) == static_cast<ssize_t>(expected.size()),
          "client recv failed");

    check(std::string_view(buffer, expected.size()) == expected, "client received wrong bytes");
}

void test_listener_and_empty_accept() {
    NativePoller poller;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller);

    check(manager.listener_registered(), "listener was not registered");

    check(manager.listener_token() != redisdb::net::InvalidToken, "listener has invalid token");

    check(manager.is_listener_event(manager.listener_token()), "listener identity not recognized");

    check(manager.connection_count() == 0, "manager has unexpected connections");

    expect_exception<std::invalid_argument>([&] { (void)manager.accept_ready(0); },
                                            "zero accept budget accepted");

    const auto empty = manager.accept_ready(1);

    check(empty.tokens.empty() && empty.would_block, "empty accept did not report WouldBlock");
}

void test_accept_read_write_and_close() {
    NativePoller poller;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller);

    UniqueFd client = connect_loopback(manager.listener_port());

    const auto listener_events = poller.wait(1000ms);

    check(has_event(listener_events, manager.listener_token(), EventMask::Read),
          "listener not notified of TCP connection");

    const auto accepted = manager.accept_ready(16);

    check(accepted.tokens.size() == 1, "one TCP client was not accepted");

    const RegistrationToken token = accepted.tokens[0];

    check(manager.connection_count() == 1, "connection count incorrect");

    auto* connection = manager.find(token);

    check(connection != nullptr, "connection lookup failed");
    check(connection->token() == token, "token mismatch");

    send_data(client.get(), "Q");

    const auto socket_events = poller.wait(1000ms);

    check(has_event(socket_events, token, EventMask::Read),
          "accepted client did not become readable");

    const auto read = connection->receive_once(16);

    check(read.bytes_read == 1 && connection->input() == "Q", "accepted connection read failed");

    connection->consume_input(1);
    connection->queue_output("OK");

    const auto write = connection->flush_once(16);

    check(write.bytes_written == 2, "accepted connection write failed");

    receive_data(client.get(), "OK");

    const int owned_fd = connection->fd();

    check(manager.close_connection(token), "connection close returned false");

    check(manager.connection_count() == 0, "closed connection remained registered");

    check(manager.find(token) == nullptr, "stale connection lookup succeeded");

    errno = 0;

    check(::fcntl(owned_fd, F_GETFD) == -1 && errno == EBADF, "accepted descriptor was not closed");

    check(!manager.close_connection(token), "duplicate close was not idempotent");

    check(manager.listener_registered(), "listener unexpectedly disabled");
}

void test_multiple_clients_and_capacity() {
    NativePoller poller;

    ConnectionManager::Limits limits;
    limits.max_connections = 2;
    limits.max_accept_per_call = 1;

    ConnectionManager manager(TcpListener::bind_loopback(0), poller, limits);

    const RegistrationToken original_listener = manager.listener_token();

    UniqueFd first = connect_loopback(manager.listener_port());
    UniqueFd second = connect_loopback(manager.listener_port());
    UniqueFd third = connect_loopback(manager.listener_port());

    const auto listener_ready = poller.wait(1000ms);

    check(has_event(listener_ready, original_listener, EventMask::Read),
          "initial listener readiness missing");

    const auto batch1 = manager.accept_ready(10);

    check(batch1.tokens.size() == 1, "accept batch limit not enforced");

    check(manager.connection_count() == 1, "first accepted connection missing");

    const auto batch2 = manager.accept_ready(10);

    check(batch2.tokens.size() == 1 && batch2.capacity_reached, "capacity was not reported");

    check(manager.connection_count() == 2, "maximum connection count incorrect");

    check(!manager.listener_registered(), "listener was not suspended at capacity");

    check(!manager.is_listener_event(original_listener), "old listener token remained active");

    const auto blocked = manager.accept_ready(10);

    check(blocked.capacity_reached && blocked.tokens.empty(), "manager accepted beyond capacity");

    const RegistrationToken first_token = batch1.tokens[0];

    const RegistrationToken second_token = batch2.tokens[0];

    check(first_token != second_token, "duplicate connection tokens");

    check(manager.close_connection(first_token), "first close failed");

    check(manager.connection_count() == 1, "capacity was not released");

    check(manager.listener_registered(), "listener was not resumed");

    const RegistrationToken resumed_listener = manager.listener_token();

    check(resumed_listener != original_listener, "listener registration token was reused");

    check(!manager.is_listener_event(original_listener), "stale listener identity recognized");

    const auto resumed_ready = poller.wait(1000ms);

    check(has_event(resumed_ready, resumed_listener, EventMask::Read),
          "resumed listener readiness missing");

    const auto batch3 = manager.accept_ready(10);

    check(batch3.tokens.size() == 1 && batch3.capacity_reached,
          "queued third client was not accepted");

    const RegistrationToken third_token = batch3.tokens[0];

    check(third_token != first_token && third_token != second_token &&
              third_token != resumed_listener,
          "registration token was reused");

    check(manager.find(third_token) != nullptr, "third client was not registered");

    check(manager.close_connection(second_token), "second close failed");

    check(manager.close_connection(third_token), "third close failed");

    check(manager.connection_count() == 0, "all connections were not removed");

    check(manager.listener_registered(), "listener was not left active");
}

void test_invalid_limits() {
    NativePoller poller;

    ConnectionManager::Limits limits;

    limits.max_connections = 0;

    expect_exception<std::invalid_argument>(
        [&] { ConnectionManager invalid(TcpListener::bind_loopback(0), poller, limits); },
        "zero max connections accepted");

    limits.max_connections = 1;
    limits.max_accept_per_call = 0;

    expect_exception<std::invalid_argument>(
        [&] { ConnectionManager invalid(TcpListener::bind_loopback(0), poller, limits); },
        "zero accept batch accepted");

    limits.max_accept_per_call = 1;
    limits.connection.max_input_bytes = 0;

    expect_exception<std::invalid_argument>(
        [&] { ConnectionManager invalid(TcpListener::bind_loopback(0), poller, limits); },
        "zero client input limit accepted");
}

void test_manager_destruction() {
    NativePoller poller;

    int listener_fd = -1;
    int connection_fd = -1;

    {
        ConnectionManager manager(TcpListener::bind_loopback(0), poller);

        listener_fd = manager.listener_fd();

        UniqueFd client = connect_loopback(manager.listener_port());

        const auto ready = poller.wait(1000ms);

        check(has_event(ready, manager.listener_token(), EventMask::Read),
              "destructor setup listener not ready");

        const auto result = manager.accept_ready(1);

        check(result.tokens.size() == 1, "destructor test client was not accepted");

        const auto* connection = manager.find(result.tokens[0]);

        check(connection != nullptr, "destructor test lookup failed");

        connection_fd = connection->fd();
    }

    errno = 0;

    check(::fcntl(connection_fd, F_GETFD) == -1 && errno == EBADF,
          "manager destructor leaked client socket");

    errno = 0;

    check(::fcntl(listener_fd, F_GETFD) == -1 && errno == EBADF,
          "manager destructor leaked listener");
}

} // namespace

int main() {
    try {
        test_listener_and_empty_accept();
        test_accept_read_write_and_close();
        test_multiple_clients_and_capacity();
        test_invalid_limits();
        test_manager_destruction();

        std::cout << "ConnectionManager integration tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ConnectionManager test failed: " << error.what() << '\n';

        return 1;
    }
}
