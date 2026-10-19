#include "redisdb/net/EpollPoller.hpp"
#include "redisdb/net/TcpListener.hpp"
#include "redisdb/net/UniqueFd.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using redisdb::net::EpollPoller;
using redisdb::net::EventMask;
using redisdb::net::ReadyEvent;
using redisdb::net::RegistrationToken;
using redisdb::net::TcpListener;
using redisdb::net::UniqueFd;

using namespace std::chrono_literals;

static_assert(std::is_base_of_v<redisdb::net::EventPoller, EpollPoller>);

static_assert(!std::is_copy_constructible_v<EpollPoller>);

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Action>
void expect_exception(Action&& action, const char* message) {
    bool caught = false;

    try {
        std::forward<Action>(action)();
    } catch (const Exception&) {
        caught = true;
    }

    check(caught, message);
}

[[nodiscard]] bool contains_event(const std::vector<ReadyEvent>& events, RegistrationToken token,
                                  EventMask mask) {
    for (const auto& event : events) {
        if (event.token == token && redisdb::net::contains_all(event.events, mask)) {
            return true;
        }
    }

    return false;
}

struct SocketPair {
    UniqueFd first;
    UniqueFd second;

    SocketPair() {
        int sockets[2] = {-1, -1};

        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == -1) {
            throw std::system_error(errno, std::generic_category(), "socketpair");
        }

        first.reset(sockets[0]);
        second.reset(sockets[1]);
    }
};

void send_byte(int fd, char value) { check(::send(fd, &value, 1, 0) == 1, "send failed"); }

void receive_byte(int fd, char expected) {
    char received = '\0';

    check(::recv(fd, &received, 1, 0) == 1, "recv failed");

    check(received == expected, "payload mismatch");
}

[[nodiscard]] UniqueFd connect_loopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    UniqueFd client(fd);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                  static_cast<socklen_t>(sizeof(address))) == -1) {
        throw std::system_error(errno, std::generic_category(), "connect");
    }

    return client;
}

void test_descriptor_ownership() {
    SocketPair sockets;
    int epoll_fd = -1;

    {
        EpollPoller poller;

        epoll_fd = poller.native_handle();

        check(epoll_fd >= 0, "invalid epoll handle");

        const int flags = ::fcntl(epoll_fd, F_GETFD);

        check(flags != -1, "epoll fcntl failed");

        check((flags & FD_CLOEXEC) != 0, "epoll descriptor missing FD_CLOEXEC");

        poller.add(sockets.first.get(), EventMask::Read, 101);

        poller.remove(sockets.first.get());
    }

    errno = 0;

    check(::fcntl(epoll_fd, F_GETFD) == -1 && errno == EBADF, "epoll descriptor was not closed");

    check(::fcntl(sockets.first.get(), F_GETFD) != -1, "poller incorrectly closed client socket");

    check(::fcntl(sockets.second.get(), F_GETFD) != -1, "poller incorrectly closed peer socket");
}

void test_read_write_and_modify() {
    SocketPair sockets;
    EpollPoller poller;

    poller.add(sockets.first.get(), EventMask::Read, 201);

    check(poller.wait(0ms).empty(), "idle read interest unexpectedly ready");

    send_byte(sockets.second.get(), 'A');

    check(contains_event(poller.wait(1000ms), 201, EventMask::Read), "read readiness missing");

    receive_byte(sockets.first.get(), 'A');

    poller.modify(sockets.first.get(), EventMask::Write);

    check(contains_event(poller.wait(1000ms), 201, EventMask::Write), "write readiness missing");

    poller.modify(sockets.first.get(), EventMask::Read | EventMask::Write);

    send_byte(sockets.second.get(), 'B');

    const auto both = poller.wait(1000ms);

    check(contains_event(both, 201, EventMask::Read), "combined read interest missing");

    check(contains_event(both, 201, EventMask::Write), "combined write interest missing");

    receive_byte(sockets.first.get(), 'B');

    poller.remove(sockets.first.get());
    poller.remove(sockets.first.get());
}

void test_multiple_registrations() {
    SocketPair first;
    SocketPair second;
    EpollPoller poller;

    poller.add(first.first.get(), EventMask::Read, 301);
    poller.add(second.first.get(), EventMask::Read, 302);

    send_byte(first.second.get(), 'X');
    send_byte(second.second.get(), 'Y');

    const auto ready = poller.wait(1000ms);

    check(contains_event(ready, 301, EventMask::Read), "first registration not ready");

    check(contains_event(ready, 302, EventMask::Read), "second registration not ready");

    receive_byte(first.first.get(), 'X');
    receive_byte(second.first.get(), 'Y');

    poller.remove(first.first.get());
    poller.remove(second.first.get());
}

void test_validation_and_failed_add() {
    SocketPair sockets;
    EpollPoller poller;

    expect_exception<std::invalid_argument>([&] { poller.add(-1, EventMask::Read, 1); },
                                            "negative descriptor accepted");

    expect_exception<std::invalid_argument>(
        [&] { poller.add(sockets.first.get(), EventMask::Error, 1); },
        "output-only interest accepted");

    expect_exception<std::invalid_argument>(
        [&] { poller.add(sockets.first.get(), EventMask::Read, 0); },
        "zero registration token accepted");

    poller.add(sockets.first.get(), EventMask::Read, 401);

    expect_exception<std::logic_error>(
        [&] { poller.add(sockets.first.get(), EventMask::Write, 402); },
        "duplicate descriptor accepted");

    expect_exception<std::logic_error>(
        [&] { poller.add(sockets.second.get(), EventMask::Read, 401); },
        "duplicate live token accepted");

    expect_exception<std::logic_error>(
        [&] { poller.modify(sockets.second.get(), EventMask::Read); },
        "modify on absent descriptor accepted");

    expect_exception<std::invalid_argument>(
        [&] { poller.modify(sockets.first.get(), EventMask::Hangup); },
        "invalid modification accepted");

    expect_exception<std::invalid_argument>([&] { (void)poller.wait(-2ms); },
                                            "invalid negative timeout accepted");

    const int invalid_fd = ::dup(sockets.first.get());

    check(invalid_fd >= 0, "dup failed");

    check(::close(invalid_fd) == 0, "close failed");

    expect_exception<std::system_error>([&] { poller.add(invalid_fd, EventMask::Read, 403); },
                                        "closed FD registration did not fail");

    // Failed ADD must not retain its token or FD.
    poller.add(sockets.second.get(), EventMask::Read, 403);

    poller.remove(sockets.first.get());
    poller.remove(sockets.second.get());
}

void test_registration_tokens_and_removal() {
    SocketPair sockets;
    EpollPoller poller;

    poller.add(sockets.first.get(), EventMask::Read, 501);

    send_byte(sockets.second.get(), 'S');

    const auto old_batch = poller.wait(1000ms);

    check(contains_event(old_batch, 501, EventMask::Read), "old token missing");

    receive_byte(sockets.first.get(), 'S');

    poller.remove(sockets.first.get());

    poller.add(sockets.first.get(), EventMask::Read, 502);

    send_byte(sockets.second.get(), 'N');

    const auto new_batch = poller.wait(1000ms);

    check(contains_event(new_batch, 502, EventMask::Read), "new token missing");

    check(!contains_event(new_batch, 501, EventMask::Read), "removed registration token returned");

    // An already-returned old batch remains stale.
    check(old_batch.front().token != new_batch.front().token, "registration identity was reused");

    receive_byte(sockets.first.get(), 'N');

    poller.remove(sockets.first.get());
}

void test_timeout_and_infinite_wait() {
    EpollPoller poller;

    check(poller.wait(0ms).empty(), "empty immediate poll produced events");

    const auto start = std::chrono::steady_clock::now();

    check(poller.wait(30ms).empty(), "empty finite wait produced events");

    check(std::chrono::steady_clock::now() - start >= 10ms,
          "finite wait returned unexpectedly early");

    SocketPair sockets;

    poller.add(sockets.first.get(), EventMask::Read, 601);

    std::atomic<int> send_result{-1};

    std::jthread writer([&] {
        std::this_thread::sleep_for(20ms);

        const char value = 'W';

        send_result.store(static_cast<int>(::send(sockets.second.get(), &value, 1, 0)));
    });

    const auto ready = poller.wait(-1ms);

    writer.join();

    check(send_result.load() == 1, "writer send failed");

    check(contains_event(ready, 601, EventMask::Read), "infinite wait did not wake");

    receive_byte(sockets.first.get(), 'W');

    poller.remove(sockets.first.get());
}

void test_hangup() {
    SocketPair sockets;
    EpollPoller poller;

    poller.add(sockets.first.get(), EventMask::Read, 701);

    check(::shutdown(sockets.second.get(), SHUT_WR) == 0, "shutdown failed");

    const auto events = poller.wait(1000ms);

    check(contains_event(events, 701, EventMask::Hangup), "peer half-close did not report hangup");

    poller.remove(sockets.first.get());
}

void test_tcp_listener_readiness() {
    TcpListener listener = TcpListener::bind_loopback(0);

    EpollPoller poller;

    poller.add(listener.fd(), EventMask::Read, 801);

    UniqueFd client = connect_loopback(listener.port());

    check(contains_event(poller.wait(1000ms), 801, EventMask::Read),
          "TCP listener readiness missing");

    auto accepted = listener.accept_one();

    check(accepted.has_value(), "TCP connection was not accepted");

    poller.add(accepted->get(), EventMask::Read, 802);

    send_byte(client.get(), 'T');

    check(contains_event(poller.wait(1000ms), 802, EventMask::Read),
          "accepted TCP socket not readable");

    receive_byte(accepted->get(), 'T');

    poller.remove(accepted->get());
    poller.remove(listener.fd());
}

} // namespace

int main() {
    try {
        test_descriptor_ownership();
        test_read_write_and_modify();
        test_multiple_registrations();
        test_validation_and_failed_add();
        test_registration_tokens_and_removal();
        test_timeout_and_infinite_wait();
        test_hangup();
        test_tcp_listener_readiness();

        std::cout << "Native epoll integration tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native epoll test failed: " << error.what() << '\n';

        return 1;
    }
}
