#include "redisdb/net/TcpListener.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace redisdb::net {

namespace {

[[noreturn]] void throw_errno(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void configure_descriptor(int fd) {
    const int status = ::fcntl(fd, F_GETFL);

    if (status == -1) {
        throw_errno("fcntl(F_GETFL)");
    }

    if (::fcntl(fd, F_SETFL, status | O_NONBLOCK) == -1) {
        throw_errno("fcntl(F_SETFL)");
    }

    const int descriptor_flags = ::fcntl(fd, F_GETFD);

    if (descriptor_flags == -1) {
        throw_errno("fcntl(F_GETFD)");
    }

    if (::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == -1) {
        throw_errno("fcntl(F_SETFD)");
    }
}

} // namespace

TcpListener::TcpListener(UniqueFd socket, std::uint16_t port) noexcept
    : socket_(std::move(socket)), port_(port) {}

TcpListener TcpListener::bind_loopback(std::uint16_t port, int backlog) {
    if (backlog <= 0) {
        throw std::invalid_argument("TCP listen backlog must be positive");
    }

    const int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if (raw_fd == -1) {
        throw_errno("socket(AF_INET, SOCK_STREAM)");
    }

    UniqueFd socket(raw_fd);

    configure_descriptor(socket.get());

    const int reuse_address = 1;

    if (::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                     static_cast<socklen_t>(sizeof(reuse_address))) == -1) {
        throw_errno("setsockopt(SO_REUSEADDR)");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address),
               static_cast<socklen_t>(sizeof(address))) == -1) {
        throw_errno("bind(127.0.0.1)");
    }

    if (::listen(socket.get(), backlog) == -1) {
        throw_errno("listen");
    }

    sockaddr_in local_address{};
    socklen_t address_length = static_cast<socklen_t>(sizeof(local_address));

    if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&local_address), &address_length) ==
        -1) {
        throw_errno("getsockname");
    }

    return TcpListener(std::move(socket), ntohs(local_address.sin_port));
}

std::optional<UniqueFd> TcpListener::accept_one() const {
    if (!socket_) {
        throw std::logic_error("accept_one called on an invalid listener");
    }

    for (;;) {
        const int client_fd = ::accept(socket_.get(), nullptr, nullptr);

        if (client_fd >= 0) {
            UniqueFd client(client_fd);
            configure_descriptor(client.get());

            return std::optional<UniqueFd>{std::move(client)};
        }

        const int error = errno;

        if (error == EINTR || error == ECONNABORTED) {
            continue;
        }

        if (error == EAGAIN || error == EWOULDBLOCK) {
            return std::nullopt;
        }

        throw std::system_error(error, std::generic_category(), "accept");
    }
}

} // namespace redisdb::net
