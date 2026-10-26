#include "redisdb/net/TcpListener.hpp"
#include "redisdb/net/UniqueFd.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

using redisdb::net::TcpListener;
using redisdb::net::UniqueFd;

static_assert(!std::is_copy_constructible_v<TcpListener>);
static_assert(!std::is_copy_assignable_v<TcpListener>);
static_assert(std::is_nothrow_move_constructible_v<TcpListener>);
static_assert(std::is_nothrow_move_assignable_v<TcpListener>);

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] bool is_closed(int fd) {
    errno = 0;
    return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

void check_socket_flags(int fd) {
    const int status = ::fcntl(fd, F_GETFL);
    const int flags = ::fcntl(fd, F_GETFD);

    check(status != -1, "F_GETFL failed");
    check(flags != -1, "F_GETFD failed");
    check((status & O_NONBLOCK) != 0, "socket must be non-blocking");
    check((flags & FD_CLOEXEC) != 0, "socket must be close-on-exec");
}

void wait_readable(int fd) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;

    int result;

    do {
        result = ::poll(&descriptor, 1, 2000);
    } while (result == -1 && errno == EINTR);

    check(result == 1, "poll timed out or failed");
    check((descriptor.revents & POLLIN) != 0, "socket did not become readable");
}

[[nodiscard]] UniqueFd connect_loopback(std::uint16_t port) {
    const int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if (raw_fd == -1) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    UniqueFd client(raw_fd);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                  static_cast<socklen_t>(sizeof(address))) != 0) {
        throw std::system_error(errno, std::generic_category(), "connect");
    }

    return client;
}

void test_binding_and_flags() {
    TcpListener listener = TcpListener::bind_loopback(0);

    check(listener.fd() >= 0, "invalid listener");
    check(listener.port() != 0, "missing ephemeral port");

    check_socket_flags(listener.fd());

    sockaddr_in address{};
    socklen_t length = static_cast<socklen_t>(sizeof(address));

    check(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&address), &length) == 0,
          "getsockname failed");

    check(address.sin_family == AF_INET, "listener must use IPv4");

    check(address.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "listener must bind to loopback");

    check(ntohs(address.sin_port) == listener.port(), "reported port mismatch");

    check(!listener.accept_one().has_value(), "empty accept should return nullopt");
}

void test_explicit_binding() {
    for (const auto literal : {"127.0.0.1", "0.0.0.0"}) {
        auto listener = TcpListener::bind_ipv4(literal, 0);
        check_socket_flags(listener.fd());
        sockaddr_in bound{};
        socklen_t size = sizeof(bound);
        check(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&bound), &size) == 0,
              "explicit bind getsockname");
        check(bound.sin_addr.s_addr ==
                  htonl(std::string_view(literal) == "127.0.0.1" ? INADDR_LOOPBACK : INADDR_ANY),
              "explicit bind address mismatch");
        auto client = connect_loopback(listener.port());
        wait_readable(listener.fd());
        check(listener.accept_one().has_value(), "explicit bind accept");
    }
    for (const auto invalid : {"localhost", "::1", "127.1", "127.0.0.2", "0.0.0.0 ", ""}) {
        bool rejected = false;
        try {
            (void)TcpListener::bind_ipv4(invalid, 0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "invalid bind address accepted");
    }
}

void test_single_connection_and_io() {
    TcpListener listener = TcpListener::bind_loopback(0, 8);

    UniqueFd client = connect_loopback(listener.port());

    wait_readable(listener.fd());

    auto accepted = listener.accept_one();

    check(accepted.has_value(), "accept failed");

    check_socket_flags(accepted->get());

    const char message = 'R';

    check(::send(client.get(), &message, 1, 0) == 1, "client send failed");

    wait_readable(accepted->get());

    char received = '\0';

    check(::recv(accepted->get(), &received, 1, 0) == 1, "server recv failed");

    check(received == message, "payload mismatch");

    check(!listener.accept_one().has_value(), "unexpected additional connection");
}

void test_multiple_connections() {
    constexpr std::array<char, 4> messages{'A', 'B', 'C', 'D'};

    TcpListener listener = TcpListener::bind_loopback(0, 16);

    std::vector<UniqueFd> clients;
    std::vector<UniqueFd> accepted;

    clients.reserve(messages.size());
    accepted.reserve(messages.size());

    for (char message : messages) {
        clients.push_back(connect_loopback(listener.port()));

        check(::send(clients.back().get(), &message, 1, 0) == 1, "queued send failed");
    }

    for (std::size_t i = 0; i < messages.size(); ++i) {
        wait_readable(listener.fd());

        auto connection = listener.accept_one();

        check(connection.has_value(), "queued accept failed");

        check_socket_flags(connection->get());

        accepted.push_back(std::move(*connection));
    }

    std::set<char> received;

    for (auto& connection : accepted) {
        wait_readable(connection.get());

        char message = '\0';

        check(::recv(connection.get(), &message, 1, 0) == 1, "queued recv failed");

        received.insert(message);
    }

    check(received.size() == messages.size(), "missing or duplicate payload");

    for (char message : messages) {
        check(received.contains(message), "expected payload missing");
    }

    check(!listener.accept_one().has_value(), "unexpected extra client");
}

void test_move_and_cleanup() {
    int owned_fd = -1;

    {
        TcpListener first = TcpListener::bind_loopback(0);

        const int original_fd = first.fd();
        const std::uint16_t original_port = first.port();

        TcpListener second(std::move(first));

        check(first.fd() == -1, "move source owns descriptor");

        check(first.port() == 0, "moved-from listener reports port");

        check(second.fd() == original_fd, "move construction failed");

        check(second.port() == original_port, "move changed port");

        TcpListener third = TcpListener::bind_loopback(0);

        const int previous_fd = third.fd();

        third = std::move(second);

        check(is_closed(previous_fd), "move assignment leaked socket");

        check(second.fd() == -1, "move assignment source not empty");

        check(third.fd() == original_fd, "move assignment lost descriptor");

        owned_fd = third.fd();
    }

    check(is_closed(owned_fd), "destructor did not close listener");
}

void test_error_paths() {
    bool rejected_backlog = false;

    try {
        auto listener = TcpListener::bind_loopback(0, 0);
        (void)listener;
    } catch (const std::invalid_argument&) {
        rejected_backlog = true;
    }

    check(rejected_backlog, "invalid backlog not rejected");

    TcpListener listener = TcpListener::bind_loopback(0);

    bool address_in_use = false;

    try {
        auto duplicate = TcpListener::bind_loopback(listener.port());
        (void)duplicate;
    } catch (const std::system_error& error) {
        address_in_use = error.code().value() == EADDRINUSE;
    }

    check(address_in_use, "duplicate binding not rejected");

    TcpListener moved(std::move(listener));

    bool rejected_invalid_listener = false;

    try {
        (void)listener.accept_one();
    } catch (const std::logic_error&) {
        rejected_invalid_listener = true;
    }

    check(rejected_invalid_listener, "invalid listener not rejected");

    check(moved.fd() >= 0, "valid listener lost after move");
}

} // namespace

int main() {
    try {
        test_binding_and_flags();
        test_explicit_binding();
        test_single_connection_and_io();
        test_multiple_connections();
        test_move_and_cleanup();
        test_error_paths();

        std::cout << "TCP listener integration tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TCP listener test failed: " << error.what() << '\n';

        return 1;
    }
}
