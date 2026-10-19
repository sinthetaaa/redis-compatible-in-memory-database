#include "redisdb/net/Connection.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace redisdb::net {

namespace {

[[noreturn]] void throw_errno(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void validate_socket(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);

    if (flags == -1) {
        throw_errno("fcntl(F_GETFL) connection");
    }

    if ((flags & O_NONBLOCK) == 0) {
        throw std::invalid_argument("connection socket must be non-blocking");
    }

#if defined(SO_NOSIGPIPE)
    // macOS: prevent SIGPIPE when writing to a peer
    // that has disconnected.
    const int enabled = 1;

    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                     static_cast<socklen_t>(sizeof(enabled))) == -1) {
        throw_errno("setsockopt(SO_NOSIGPIPE)");
    }
#endif
}

[[nodiscard]] int safe_send_flags() noexcept {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

} // namespace

Connection::Connection(UniqueFd socket, RegistrationToken token)
    : Connection(std::move(socket), token, Limits{}) {}

Connection::Connection(UniqueFd socket, RegistrationToken token, Limits limits)
    : socket_(std::move(socket)), token_(token), limits_(limits) {
    if (!socket_) {
        throw std::invalid_argument("connection requires an owned socket");
    }

    if (token_ == InvalidToken) {
        throw std::invalid_argument("connection requires a nonzero token");
    }

    if (limits_.max_input_bytes == 0 || limits_.max_output_bytes == 0) {
        throw std::invalid_argument("connection buffer limits must be positive");
    }

    validate_socket(socket_.get());
}

Connection::ReadResult Connection::receive_once(std::size_t budget) {
    if (!socket_) {
        throw std::logic_error("cannot read from closed connection");
    }

    if (budget == 0) {
        throw std::invalid_argument("receive budget must be positive");
    }

    if (received_eof_) {
        return {.peer_closed = true};
    }

    const std::size_t available = limits_.max_input_bytes - input_.size();

    if (available == 0) {
        return {.input_full = true};
    }

    std::array<char, 8192> buffer{};

    const std::size_t request = std::min({budget, available, buffer.size()});

    for (;;) {
        const auto received = ::recv(socket_.get(), buffer.data(), request, 0);

        if (received > 0) {
            const auto count = static_cast<std::size_t>(received);

            input_.append(buffer.data(), count);

            last_activity_ = std::chrono::steady_clock::now();

            return {.bytes_read = count, .input_full = input_.size() == limits_.max_input_bytes};
        }

        if (received == 0) {
            received_eof_ = true;
            return {.peer_closed = true};
        }

        const int error = errno;

        if (error == EINTR) {
            continue;
        }

        if (error == EAGAIN || error == EWOULDBLOCK) {
            return {.would_block = true};
        }

        if (error == ECONNRESET) {
            received_eof_ = true;
            return {.peer_closed = true};
        }

        throw std::system_error(error, std::generic_category(), "recv connection");
    }
}

void Connection::consume_input(std::size_t count) {
    if (count > input_.size()) {
        throw std::out_of_range("consume exceeds buffered input");
    }

    input_.erase(0, count);
}

void Connection::queue_output(std::string_view data) {
    if (!socket_) {
        throw std::logic_error("cannot queue output on closed connection");
    }

    const auto pending = pending_output_bytes();

    if (data.size() > limits_.max_output_bytes - pending) {
        throw std::length_error("connection output buffer limit exceeded");
    }

    if (data.empty()) {
        return;
    }

    // Compact bytes already written before appending.
    if (output_offset_ != 0) {
        output_.erase(0, output_offset_);
        output_offset_ = 0;
    }

    output_.append(data.data(), data.size());
}

Connection::WriteResult Connection::flush_once(std::size_t budget) {
    if (!socket_) {
        throw std::logic_error("cannot write to closed connection");
    }

    if (budget == 0) {
        throw std::invalid_argument("flush budget must be positive");
    }

    const auto pending = pending_output_bytes();

    if (pending == 0) {
        return {};
    }

    const auto request = std::min(budget, pending);

    for (;;) {
        const auto written =
            ::send(socket_.get(), output_.data() + output_offset_, request, safe_send_flags());

        if (written > 0) {
            const auto count = static_cast<std::size_t>(written);

            output_offset_ += count;

            last_activity_ = std::chrono::steady_clock::now();

            if (output_offset_ == output_.size()) {
                output_.clear();
                output_offset_ = 0;
            }

            return {.bytes_written = count};
        }

        if (written == 0) {
            throw std::runtime_error("send made no progress");
        }

        const int error = errno;

        if (error == EINTR) {
            continue;
        }

        if (error == EAGAIN || error == EWOULDBLOCK) {
            return {.would_block = true};
        }

        if (error == EPIPE || error == ECONNRESET || error == ENOTCONN) {
            return {.peer_disconnected = true};
        }

        throw std::system_error(error, std::generic_category(), "send connection");
    }
}

void Connection::close() noexcept { socket_.reset(); }

} // namespace redisdb::net
