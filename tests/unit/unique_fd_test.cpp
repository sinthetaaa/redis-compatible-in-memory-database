#include "redisdb/net/UniqueFd.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>

using redisdb::net::UniqueFd;

static_assert(!std::is_copy_constructible_v<UniqueFd>);
static_assert(!std::is_copy_assignable_v<UniqueFd>);
static_assert(std::is_nothrow_move_constructible_v<UniqueFd>);
static_assert(std::is_nothrow_move_assignable_v<UniqueFd>);
static_assert(std::is_nothrow_destructible_v<UniqueFd>);

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] bool is_open(int fd) {
    errno = 0;
    return ::fcntl(fd, F_GETFD) != -1;
}

struct SocketPair {
    UniqueFd first;
    UniqueFd second;

    SocketPair() {
        int fds[2] = {-1, -1};

        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            throw std::runtime_error("socketpair failed");
        }

        first.reset(fds[0]);
        second.reset(fds[1]);
    }
};

void test_empty() {
    UniqueFd fd;

    check(!fd, "default descriptor must be invalid");
    check(fd.get() == -1, "default descriptor sentinel");
    check(fd.release() == -1, "release on empty owner");

    fd.reset();

    check(!fd, "reset on empty owner");
}

void test_socket_io_and_scope_cleanup() {
    int closed_fd = -1;

    {
        SocketPair pair;

        check(pair.first && pair.second, "both sockets owned");
        check(is_open(pair.first.get()), "first socket open");
        check(is_open(pair.second.get()), "second socket open");

        char send_byte = 'R';

        check(::send(pair.first.get(), &send_byte, 1, 0) == 1, "send failed");

        char receive_byte = '\0';

        check(::recv(pair.second.get(), &receive_byte, 1, 0) == 1, "recv failed");

        check(receive_byte == send_byte, "socket payload mismatch");

        closed_fd = pair.first.get();
    }

    check(!is_open(closed_fd), "destructor did not close socket");

    check(errno == EBADF, "closed descriptor must be EBADF");
}

void test_move_construction_and_release() {
    SocketPair pair;

    const int original_fd = pair.first.get();

    UniqueFd owner(std::move(pair.first));

    check(!pair.first, "moved-from owner must be empty");
    check(owner.get() == original_fd, "move construction changed descriptor");
    check(is_open(original_fd), "transferred descriptor must remain open");

    const int released_fd = owner.release();

    check(!owner, "release must clear owner");
    check(released_fd == original_fd, "release must return original descriptor");
    check(is_open(released_fd), "release must not close descriptor");

    check(::close(released_fd) == 0, "manual close after release failed");
    check(!is_open(released_fd), "released descriptor still open after close");
}

void test_move_assignment_closes_old() {
    SocketPair pair;

    const int old_fd = pair.first.get();
    const int transferred_fd = pair.second.get();

    pair.first = std::move(pair.second);

    check(!pair.second, "moved-from assignment source must be empty");
    check(pair.first.get() == transferred_fd, "move assignment failed");
    check(!is_open(old_fd), "move assignment leaked previous fd");
    check(is_open(transferred_fd), "move assignment closed incoming fd");

    UniqueFd& same = pair.first;
    pair.first = std::move(same);

    check(pair.first.get() == transferred_fd, "self-move lost ownership");
}

void test_reset_replaces_and_is_idempotent() {
    SocketPair pair;

    const int old_fd = pair.first.get();
    const int replacement_fd = pair.second.release();

    pair.first.reset(replacement_fd);

    check(!is_open(old_fd), "reset must close previous fd");
    check(pair.first.get() == replacement_fd, "reset must adopt replacement");

    pair.first.reset(replacement_fd);

    check(is_open(replacement_fd), "reset with same fd must do nothing");

    pair.first.reset();

    check(!pair.first, "reset must invalidate owner");
    check(!is_open(replacement_fd), "reset must close owned fd");

    pair.first.reset();
}

} // namespace

int main() {
    try {
        test_empty();
        test_socket_io_and_scope_cleanup();
        test_move_construction_and_release();
        test_move_assignment_closes_old();
        test_reset_replaces_and_is_idempotent();

        std::cout << "UniqueFd ownership tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UniqueFd test failed: " << error.what() << '\n';

        return 1;
    }
}
