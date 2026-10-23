#pragma once

#include <chrono>

namespace redisdb::storage {

// Phase 5A: time source only. Expiration metadata and policy belong to 5B-5H.
// Monotonic time is suitable for elapsed-time checks and in-process scheduling;
// wall time is needed for absolute deadlines which must survive process restarts.
// Never serialize a steady_clock::time_point as a persistent deadline.
using MonotonicTimePoint = std::chrono::steady_clock::time_point;
using WallTimePoint = std::chrono::system_clock::time_point;

// Borrow the clock via const Clock&; its owner must outlive consumers.
// Clock implementations must keep now() nonthrowing. Test clocks can control
// both domains independently to avoid sleeps or dependence on machine time.
class Clock {
  public:
    virtual ~Clock() = default;

    [[nodiscard]] virtual MonotonicTimePoint monotonic_now() const noexcept = 0;
    [[nodiscard]] virtual WallTimePoint wall_now() const noexcept = 0;
};

// Production time source. This class does not store any expiration state.
class SystemClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override;
    [[nodiscard]] WallTimePoint wall_now() const noexcept override;
};

} // namespace redisdb::storage
