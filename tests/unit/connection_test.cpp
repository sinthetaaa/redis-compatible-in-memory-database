#include "redisdb/net/Connection.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

using redisdb::net::Connection;
using redisdb::net::UniqueFd;

static_assert(!std::is_copy_constructible_v<Connection>);
static_assert(!std::is_move_constructible_v<Connection>);

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

void make_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);

    check(flags >= 0, "fcntl get flags failed");

    check(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "fcntl set nonblocking failed");
}

struct SocketPair {
    UniqueFd first;
    UniqueFd second;

    SocketPair() {
        int raw[2] = {-1, -1};

        check(::socketpair(AF_UNIX, SOCK_STREAM, 0, raw) == 0, "socketpair creation failed");

        first.reset(raw[0]);
        second.reset(raw[1]);

        make_nonblocking(first.get());
        make_nonblocking(second.get());
    }
};

void send_data(int fd, std::string_view bytes) {
    check(::send(fd, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()),
          "test send failed");
}

void receive_exact(int fd, std::string_view expected) {
    char buffer[64]{};

    check(expected.size() <= sizeof(buffer), "test payload too large");

    const auto result = ::recv(fd, buffer, expected.size(), 0);

    check(result == static_cast<ssize_t>(expected.size()), "test recv length mismatch");

    check(std::string_view(buffer, expected.size()) == expected, "test recv payload mismatch");
}

void test_constructor_validation() {
    expect_exception<std::invalid_argument>([] { Connection invalid(UniqueFd{}, 1); },
                                            "invalid socket was accepted");

    {
        SocketPair pair;

        expect_exception<std::invalid_argument>(
            [&] { Connection invalid(std::move(pair.first), 0); },
            "zero registration token was accepted");
    }

    {
        SocketPair pair;

        expect_exception<std::invalid_argument>(
            [&] { Connection invalid(std::move(pair.first), 2, Connection::Limits{0, 4}); },
            "zero input limit was accepted");
    }

    {
        SocketPair pair;

        expect_exception<std::invalid_argument>(
            [&] { Connection invalid(std::move(pair.first), 3, Connection::Limits{4, 0}); },
            "zero output limit was accepted");
    }

    {
        SocketPair pair;

        const int flags = ::fcntl(pair.first.get(), F_GETFL);

        check(flags >= 0, "fcntl failed");

        check(::fcntl(pair.first.get(), F_SETFL, flags & ~O_NONBLOCK) == 0,
              "unable to clear nonblocking flag");

        expect_exception<std::invalid_argument>(
            [&] { Connection invalid(std::move(pair.first), 4); }, "blocking socket was accepted");
    }
}

void test_partial_receive_and_consume() {
    SocketPair pair;

    Connection connection(std::move(pair.first), 101);

    check(connection.is_open(), "connection not open");

    check(connection.token() == 101, "registration token changed");

    send_data(pair.second.get(), "abcd");

    const auto first = connection.receive_once(2);

    check(first.bytes_read == 2, "first receive budget ignored");

    check(connection.input() == "ab", "first input payload incorrect");

    const auto second = connection.receive_once(2);

    check(second.bytes_read == 2, "second receive budget ignored");

    check(connection.input() == "abcd", "combined input payload incorrect");

    connection.consume_input(2);

    check(connection.input() == "cd", "input consumption incorrect");

    expect_exception<std::out_of_range>([&] { connection.consume_input(3); },
                                        "over-consumption was accepted");

    check(connection.input() == "cd", "failed consume modified input");

    connection.consume_input(2);

    check(connection.input().empty(), "input was not fully consumed");

    const auto idle = connection.receive_once(16);

    check(idle.would_block, "idle socket did not return WouldBlock");
}

void test_input_limit() {
    SocketPair pair;

    Connection connection(std::move(pair.first), 201, Connection::Limits{3, 16});

    send_data(pair.second.get(), "abcd");

    const auto first = connection.receive_once(100);

    check(first.bytes_read == 3, "input limit was exceeded");

    check(first.input_full, "full input buffer not reported");

    check(connection.input() == "abc", "limited input incorrect");

    const auto full = connection.receive_once(100);

    check(full.input_full, "full buffer did not block further reads");

    connection.consume_input(2);

    const auto second = connection.receive_once(100);

    check(second.bytes_read == 1, "remaining byte was not read");

    check(connection.input() == "cd", "input after consumption incorrect");
}

void test_output_budget_and_limit() {
    SocketPair pair;

    Connection connection(std::move(pair.first), 301, Connection::Limits{16, 5});

    connection.queue_output("hello");

    check(connection.pending_output_bytes() == 5, "pending bytes incorrect");

    check(connection.wants_write(), "write interest should be requested");

    expect_exception<std::length_error>([&] { connection.queue_output("!"); },
                                        "output cap not enforced");

    check(connection.pending_output_bytes() == 5, "failed queue changed pending output");

    const auto first = connection.flush_once(2);

    check(first.bytes_written == 2, "write budget not enforced");

    receive_exact(pair.second.get(), "he");

    check(connection.pending_output_bytes() == 3, "partial output accounting incorrect");

    connection.queue_output("!");

    check(connection.pending_output_bytes() == 4, "output compaction or append failed");

    const auto second = connection.flush_once(16);

    check(second.bytes_written == 4, "queued output not fully flushed");

    receive_exact(pair.second.get(), "llo!");

    check(!connection.wants_write(), "write interest not cleared");

    check(connection.pending_output_bytes() == 0, "completed output not cleared");

    const auto empty = connection.flush_once(10);

    check(empty.bytes_written == 0, "empty flush should write zero bytes");
}

void test_half_close() {
    SocketPair pair;

    Connection connection(std::move(pair.first), 401);

    check(::shutdown(pair.second.get(), SHUT_WR) == 0, "peer shutdown failed");

    const auto result = connection.receive_once(16);

    check(result.peer_closed, "peer EOF was not recognized");

    check(connection.received_eof(), "EOF state was not retained");

    // A half-closed peer can still receive output.
    connection.queue_output("ok");

    const auto flushed = connection.flush_once(16);

    check(flushed.bytes_written == 2, "reply after read EOF failed");

    receive_exact(pair.second.get(), "ok");
}

void test_disconnected_peer_write() {
    SocketPair pair;

    Connection connection(std::move(pair.first), 501);

    pair.second.reset();

    connection.queue_output("abc");

    const auto result = connection.flush_once(16);

    check(result.peer_disconnected, "closed peer not detected on write");
}

void test_cleanup_and_closed_guards() {
    SocketPair pair;

    const int raw_fd = pair.first.get();

    {
        Connection connection(std::move(pair.first), 601);

        expect_exception<std::invalid_argument>([&] { (void)connection.receive_once(0); },
                                                "zero receive budget accepted");

        expect_exception<std::invalid_argument>([&] { (void)connection.flush_once(0); },
                                                "zero flush budget accepted");

        connection.close();

        check(!connection.is_open(), "close did not invalidate socket");

        expect_exception<std::logic_error>([&] { connection.queue_output("no"); },
                                           "closed connection queued output");

        expect_exception<std::logic_error>([&] { (void)connection.receive_once(1); },
                                           "closed connection accepted read");

        expect_exception<std::logic_error>([&] { (void)connection.flush_once(1); },
                                           "closed connection accepted write");
    }

    errno = 0;

    check(::fcntl(raw_fd, F_GETFD) == -1 && errno == EBADF, "owned socket was not closed");
}

} // namespace

int main() {
    try {
        test_constructor_validation();
        test_partial_receive_and_consume();
        test_input_limit();
        test_output_budget_and_limit();
        test_half_close();
        test_disconnected_peer_write();
        test_cleanup_and_closed_guards();

        std::cout << "Connection ownership and I/O tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Connection test failed: " << error.what() << '\n';

        return 1;
    }
}
