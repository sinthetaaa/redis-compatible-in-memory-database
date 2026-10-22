#pragma once

#include "redisdb/net/EventPoller.hpp"
#include "redisdb/net/UniqueFd.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace redisdb::net {

// A single owned, non-blocking stream connection.
//
// No parser, command executor, or event-loop dependency.
//
// Registration and removal are the responsibility
// of the future ConnectionManager.
//
// This object is deliberately neither copyable nor
// movable so its registration identity stays stable.
class Connection {
  public:
    // An accepted peer disappeared while its socket
    // was being configured. The manager may discard
    // this client without stopping other connections.
    class DisconnectedDuringSetup final : public std::system_error {
      public:
        explicit DisconnectedDuringSetup(int error)
            : std::system_error(error, std::generic_category(),
                                "accepted peer disconnected during SO_NOSIGPIPE setup") {}
    };

    struct Limits {
        std::size_t max_input_bytes = 64 * 1024;
        std::size_t max_output_bytes = 64 * 1024;
    };

    struct ReadResult {
        std::size_t bytes_read = 0;
        bool would_block = false;
        bool peer_closed = false;
        bool input_full = false;
    };

    struct WriteResult {
        std::size_t bytes_written = 0;
        bool would_block = false;
        bool peer_disconnected = false;
    };

    Connection(UniqueFd socket, RegistrationToken token);

    Connection(UniqueFd socket, RegistrationToken token, Limits limits);

    ~Connection() = default;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    [[nodiscard]] int fd() const noexcept { return socket_.get(); }

    [[nodiscard]] RegistrationToken token() const noexcept { return token_; }

    [[nodiscard]] bool is_open() const noexcept { return static_cast<bool>(socket_); }

    [[nodiscard]] bool received_eof() const noexcept { return received_eof_; }

    // QUIT requests output-first close; ConnectionManager owns FD teardown.
    void request_close_after_flush() noexcept { close_after_flush_ = true; }
    [[nodiscard]] bool closing_after_flush() const noexcept { return close_after_flush_; }

    [[nodiscard]] std::string_view input() const noexcept { return input_; }

    // Read-only input cap for protocol framing compatibility.
    [[nodiscard]] std::size_t max_input_bytes() const noexcept { return limits_.max_input_bytes; }

    // Read-only access for bounded protocol output admission.
    [[nodiscard]] std::size_t max_output_bytes() const noexcept { return limits_.max_output_bytes; }

    [[nodiscard]] std::size_t pending_output_bytes() const noexcept {
        return output_.size() - output_offset_;
    }

    [[nodiscard]] bool wants_write() const noexcept { return pending_output_bytes() != 0; }

    [[nodiscard]] std::chrono::steady_clock::time_point last_activity() const noexcept {
        return last_activity_;
    }

    // Performs at most one successful recv() call.
    // Retries EINTR; returns on EAGAIN/EWOULDBLOCK.
    //
    // The supplied budget must be positive.
    // Input never exceeds max_input_bytes.
    [[nodiscard]] ReadResult receive_once(std::size_t budget);

    // Remove already-consumed bytes from the input.
    // A future protocol parser will own this decision.
    void consume_input(std::size_t count);

    // Add pending output without exceeding the cap.
    // Throws std::length_error on overflow, leaving
    // the existing pending output unchanged.
    void queue_output(std::string_view data);

    // Performs at most one successful send() call.
    // Does not treat EAGAIN as an error.
    // Prevents SIGPIPE where supported.
    [[nodiscard]] WriteResult flush_once(std::size_t budget);

    // Only the connection owner should call close().
    // The poller must first unregister this FD.
    void close() noexcept;

  private:
    UniqueFd socket_;
    RegistrationToken token_;
    Limits limits_;

    std::string input_;
    std::string output_;
    std::size_t output_offset_ = 0;

    bool received_eof_ = false;
    bool close_after_flush_ = false;

    std::chrono::steady_clock::time_point last_activity_ = std::chrono::steady_clock::now();
};

} // namespace redisdb::net
