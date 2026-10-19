#pragma once

#include "redisdb/net/UniqueFd.hpp"

#include <cstdint>
#include <optional>

namespace redisdb::net {

// Owns a non-blocking IPv4 TCP listening socket.
//
// Phase 1B restricts binding to 127.0.0.1.
// Port 0 requests an ephemeral OS-assigned port.
//
// This class does not run an accept loop or event loop.
class TcpListener {
  public:
    [[nodiscard]] static TcpListener bind_loopback(std::uint16_t port = 6380, int backlog = 128);

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    TcpListener(TcpListener&&) noexcept = default;
    TcpListener& operator=(TcpListener&&) noexcept = default;

    [[nodiscard]] int fd() const noexcept { return socket_.get(); }

    [[nodiscard]] std::uint16_t port() const noexcept { return socket_ ? port_ : 0; }

    // Returns an owned, non-blocking client socket.
    // Returns nullopt if no connection is currently ready.
    //
    // Retries interrupted or aborted accepts.
    // Throws on unexpected errors or invalid listener state.
    [[nodiscard]] std::optional<UniqueFd> accept_one() const;

  private:
    TcpListener(UniqueFd socket, std::uint16_t port) noexcept;

    UniqueFd socket_;
    std::uint16_t port_;
};

} // namespace redisdb::net
