#pragma once

namespace redisdb::net {

// Owns exactly one POSIX file descriptor. Adopt only
// descriptors whose ownership is not shared with
// another UniqueFd or an external closer.
class UniqueFd {
  public:
    UniqueFd() noexcept = default;

    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() noexcept;

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    [[nodiscard]] int get() const noexcept { return fd_; }

    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

    // Relinquishes ownership without closing the FD.
    [[nodiscard]] int release() noexcept;

    // Adopts new_fd and closes the previous descriptor.
    // Passing the currently owned fd is a no-op.
    void reset(int new_fd = -1) noexcept;

  private:
    int fd_ = -1;
};

} // namespace redisdb::net
