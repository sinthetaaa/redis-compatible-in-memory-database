#include "redisdb/net/KqueuePoller.hpp"
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
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using redisdb::net::EventMask;
using redisdb::net::KqueuePoller;
using redisdb::net::ReadyEvent;
using redisdb::net::RegistrationToken;
using redisdb::net::TcpListener;
using redisdb::net::UniqueFd;

using namespace std::chrono_literals;

static_assert(std::is_base_of_v<redisdb::net::EventPoller, KqueuePoller>);

static_assert(!std::is_copy_constructible_v<KqueuePoller>);

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
                                  EventMask expected) {
    for (const auto& event : events) {
        if (event.token == token && redisdb::net::contains_all(event.events, expected)) {
            return true;
        }
    }

    return false;
}

void send_byte(int fd, char byte) { check(::send(fd, &byte, 1, 0) == 1, "send failed"); }

void receive_byte(int fd, char expected) {
    char received = '\0';

    check(::recv(fd, &received, 1, 0) == 1, "recv failed");

    check(received == expected, "payload mismatch");
}

struct SocketPair {
    UniqueFd first;
    UniqueFd second;

    SocketPair() {
        int raw[2] = {-1, -1};

        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, raw) != 0) {
            throw std::system_error(errno, std::generic_category(), "socketpair");
        }

        first.reset(raw[0]);
        second.reset(raw[1]);
    }
};

[[nodiscard]] UniqueFd connect_loopback(std::uint16_t port) {
    const int raw = ::socket(AF_INET, SOCK_STREAM, 0);

    if (raw == -1) {
        throw std::system_error(errno, std::generic_category(), "client socket");
    }

    UniqueFd client(raw);

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

void test_queue_ownership() {
    int closed_queue = -1;
    SocketPair sockets;

    {
        KqueuePoller poller;
        closed_queue = poller.native_handle();

        check(closed_queue >= 0, "invalid kqueue fd");

        const int flags = ::fcntl(closed_queue, F_GETFD);

        check(flags != -1, "kqueue fcntl failed");
        check((flags & FD_CLOEXEC) != 0, "kqueue missing FD_CLOEXEC");

        poller.add(sockets.first.get(), EventMask::Read, 101);

        poller.remove(sockets.first.get());
    }

    errno = 0;

    check(::fcntl(closed_queue, F_GETFD) == -1 && errno == EBADF,
          "kqueue descriptor was not closed");

    check(::fcntl(sockets.first.get(), F_GETFD) != -1, "poller closed registered socket");

    check(::fcntl(sockets.second.get(), F_GETFD) != -1, "poller closed peer socket");
}

void test_read_write_and_modify() {
    SocketPair sockets;
    KqueuePoller poller;

    poller.add(sockets.first.get(), EventMask::Read, 201);

    check(poller.wait(0ms).empty(), "idle read socket unexpectedly ready");

    send_byte(sockets.second.get(), 'A');

    const auto readable = poller.wait(1000ms);

    check(contains_event(readable, 201, EventMask::Read), "missing real read readiness");

    receive_byte(sockets.first.get(), 'A');

    poller.modify(sockets.first.get(), EventMask::Write);

    const auto writable = poller.wait(1000ms);

    check(contains_event(writable, 201, EventMask::Write), "missing real write readiness");

    poller.modify(sockets.first.get(), EventMask::Read);

    send_byte(sockets.second.get(), 'B');

    const auto second_read = poller.wait(1000ms);

    check(contains_event(second_read, 201, EventMask::Read), "read filter not restored by modify");

    receive_byte(sockets.first.get(), 'B');

    poller.remove(sockets.first.get());
    poller.remove(sockets.first.get());
}

void test_multiple_tokens() {
    SocketPair first;
    SocketPair second;
    KqueuePoller poller;

    poller.add(first.first.get(), EventMask::Read, 301);

    poller.add(second.first.get(), EventMask::Read, 302);

    send_byte(first.second.get(), 'X');
    send_byte(second.second.get(), 'Y');

    const auto events = poller.wait(1000ms);

    check(contains_event(events, 301, EventMask::Read), "first registration token missing");

    check(contains_event(events, 302, EventMask::Read), "second registration token missing");

    receive_byte(first.first.get(), 'X');
    receive_byte(second.first.get(), 'Y');

    poller.remove(first.first.get());
    poller.remove(second.first.get());
}

void test_registration_validation_and_reuse() {
    SocketPair sockets;
    KqueuePoller poller;

    expect_exception<std::invalid_argument>([&] { poller.add(-1, EventMask::Read, 1); },
                                            "negative descriptor accepted");

    expect_exception<std::invalid_argument>(
        [&] { poller.add(sockets.first.get(), EventMask::Error, 1); },
        "output-only interest accepted");

    expect_exception<std::invalid_argument>(
        [&] { poller.add(sockets.first.get(), EventMask::Read, 0); }, "invalid token accepted");

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
        "invalid interest accepted");

    expect_exception<std::invalid_argument>([&] { (void)poller.wait(-2ms); },
                                            "invalid negative wait accepted");

    poller.remove(sockets.first.get());

    // Reuse the same numerical FD with a new token.
    poller.add(sockets.first.get(), EventMask::Read, 403);

    send_byte(sockets.second.get(), 'N');

    const auto events = poller.wait(1000ms);

    check(contains_event(events, 403, EventMask::Read), "new registration token not delivered");

    receive_byte(sockets.first.get(), 'N');

    poller.remove(sockets.first.get());
}

void test_timeout_and_infinite_wait() {
    KqueuePoller poller;

    const auto started = std::chrono::steady_clock::now();

    check(poller.wait(30ms).empty(), "empty kqueue produced an event");

    const auto elapsed = std::chrono::steady_clock::now() - started;

    check(elapsed >= 10ms, "finite wait returned too early");

    SocketPair sockets;

    poller.add(sockets.first.get(), EventMask::Read, 501);

    std::atomic<int> sent{-1};

    std::jthread writer([&] {
        std::this_thread::sleep_for(20ms);

        const char byte = 'W';

        sent.store(static_cast<int>(::send(sockets.second.get(), &byte, 1, 0)));
    });

    const auto events = poller.wait(-1ms);

    writer.join();

    check(sent.load() == 1, "writer send failed");

    check(contains_event(events, 501, EventMask::Read),
          "infinite wait did not wake on socket data");

    receive_byte(sockets.first.get(), 'W');

    poller.remove(sockets.first.get());
}

void test_hangup() {
    SocketPair sockets;
    KqueuePoller poller;

    poller.add(sockets.first.get(), EventMask::Read, 601);

    sockets.second.reset();

    const auto events = poller.wait(1000ms);

    check(contains_event(events, 601, EventMask::Hangup), "peer disconnect did not report Hangup");

    poller.remove(sockets.first.get());
}

void test_tcp_listener_readiness() {
    TcpListener listener = TcpListener::bind_loopback(0);

    KqueuePoller poller;

    poller.add(listener.fd(), EventMask::Read, 701);

    UniqueFd client = connect_loopback(listener.port());

    const auto events = poller.wait(1000ms);

    check(contains_event(events, 701, EventMask::Read),
          "listener did not report incoming connection");

    auto accepted = listener.accept_one();

    check(accepted.has_value(), "listener failed to accept ready connection");

    poller.add(accepted->get(), EventMask::Read, 702);

    send_byte(client.get(), 'T');

    const auto client_events = poller.wait(1000ms);

    check(contains_event(client_events, 702, EventMask::Read),
          "accepted TCP socket did not become readable");

    receive_byte(accepted->get(), 'T');

    poller.remove(accepted->get());
    poller.remove(listener.fd());
}

} // namespace

int main() {
    try {
        test_queue_ownership();
        test_read_write_and_modify();
        test_multiple_tokens();
        test_registration_validation_and_reuse();
        test_timeout_and_infinite_wait();
        test_hangup();
        test_tcp_listener_readiness();

        std::cout << "Native kqueue integration tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native kqueue test failed: " << error.what() << '\n';

        return 1;
    }
}
