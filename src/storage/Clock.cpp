#include "redisdb/storage/Clock.hpp"

namespace redisdb::storage {

MonotonicTimePoint SystemClock::monotonic_now() const noexcept {
    return std::chrono::steady_clock::now();
}

WallTimePoint SystemClock::wall_now() const noexcept { return std::chrono::system_clock::now(); }

} // namespace redisdb::storage
