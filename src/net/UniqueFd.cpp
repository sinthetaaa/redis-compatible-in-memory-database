#include "redisdb/net/UniqueFd.hpp"

#include <unistd.h>

#include <utility>

namespace redisdb::net {

UniqueFd::~UniqueFd() noexcept { reset(); }

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }

    return *this;
}

int UniqueFd::release() noexcept { return std::exchange(fd_, -1); }

void UniqueFd::reset(int new_fd) noexcept {
    if (fd_ == new_fd) {
        return;
    }

    const int old_fd = std::exchange(fd_, new_fd);

    if (old_fd >= 0) {
        // Never retry close() after EINTR: the descriptor
        // might already have been closed and reused.
        // Destruction cannot report close errors.
        (void)::close(old_fd);
    }
}

} // namespace redisdb::net
