#include "redisdb/storage/Clock.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

using redisdb::storage::Clock;
using redisdb::storage::MonotonicTimePoint;
using redisdb::storage::SystemClock;
using redisdb::storage::WallTimePoint;

namespace {

using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// An in-test fake proves the interface is injectable; the production library
// never tracks fake time and does not include any TTL or deadline policy.
class ManualClock final : public Clock {
  public:
    ManualClock(MonotonicTimePoint monotonic, WallTimePoint wall)
        : monotonic_(monotonic), wall_(wall) {}

    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return monotonic_; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return wall_; }

    void advance_monotonic(std::chrono::milliseconds duration) { monotonic_ += duration; }
    void change_wall(std::chrono::milliseconds duration) { wall_ += duration; }
    void set_wall(WallTimePoint value) { wall_ = value; }

  private:
    MonotonicTimePoint monotonic_;
    WallTimePoint wall_;
};

// Consumer sees only a Clock&, not the concrete source. A separate domain is
// deliberately used for each observation to prevent accidental clock mixing.
struct ClockSample {
    MonotonicTimePoint monotonic;
    WallTimePoint wall;
};

[[nodiscard]] ClockSample read_clock(const Clock& source) noexcept {
    return {source.monotonic_now(), source.wall_now()};
}

void test_type_contract() {
    static_assert(std::is_abstract_v<Clock>);
    static_assert(std::is_base_of_v<Clock, SystemClock>);
    static_assert(std::is_same_v<MonotonicTimePoint, std::chrono::steady_clock::time_point>);
    static_assert(std::is_same_v<WallTimePoint, std::chrono::system_clock::time_point>);
    static_assert(!std::is_same_v<MonotonicTimePoint, WallTimePoint>);
    static_assert(noexcept(std::declval<const Clock&>().monotonic_now()));
    static_assert(noexcept(std::declval<const Clock&>().wall_now()));
}

void test_injected_fake_with_independent_time_domains() {
    const auto original_monotonic = MonotonicTimePoint{} + 15s;
    const auto original_wall = WallTimePoint{} + 14400s;
    ManualClock fake(original_monotonic, original_wall);
    const Clock& injected = fake;

    const auto start = read_clock(injected);
    check(start.monotonic == original_monotonic, "fake initial monotonic time wrong");
    check(start.wall == original_wall, "fake initial wall time wrong");

    fake.advance_monotonic(125ms);
    const auto forward = read_clock(injected);
    check(forward.monotonic - start.monotonic == 125ms, "monotonic advance mismatch");
    check(forward.wall == start.wall, "monotonic adjustment changed wall time");

    fake.change_wall(-8h);
    const auto jumped = read_clock(injected);
    check(jumped.wall == start.wall - 8h, "backward wall-clock jump was lost");
    check(jumped.monotonic == forward.monotonic, "wall-clock change affected monotonic time");

    fake.change_wall(73h);
    fake.advance_monotonic(725ms);
    const auto later = read_clock(injected);
    check(later.monotonic - start.monotonic == 850ms, "monotonic domain accumulated wrongly");
    check(later.wall - start.wall == 65h, "wall domain accumulated wrongly");

    fake.set_wall(WallTimePoint{} - 99s);
    check(injected.wall_now() == WallTimePoint{} - 99s, "direct wall injection failed");
    check(injected.monotonic_now() == later.monotonic,
          "direct wall injection perturbed monotonic time");
}

void test_polymorphic_lifetime_and_separate_instances() {
    const MonotonicTimePoint tick{};
    const WallTimePoint epoch{};
    auto clock = std::make_unique<ManualClock>(tick, epoch);
    Clock& polymorphic = *clock;
    ManualClock other(tick + 5s, epoch + 5s);
    clock->advance_monotonic(3s);
    clock->change_wall(12s);
    check(polymorphic.monotonic_now() == tick + 3s, "virtual monotonic dispatch failed");
    check(polymorphic.wall_now() == epoch + 12s, "virtual wall dispatch failed");
    check(other.monotonic_now() == tick + 5s && other.wall_now() == epoch + 5s,
          "independent injected clocks shared state");
    clock.reset(); // Destroy through Clock's polymorphic ownership boundary.
}

void test_real_clock_bracketing() {
    SystemClock real;
    const Clock& injected = real;
    for (int i = 0; i < 128; ++i) {
        const auto monotonic_before = std::chrono::steady_clock::now();
        const auto sample_monotonic = injected.monotonic_now();
        const auto monotonic_after = std::chrono::steady_clock::now();
        check(monotonic_before <= sample_monotonic && sample_monotonic <= monotonic_after,
              "system monotonic clock not bracketed by steady_clock");

        const auto wall_before = std::chrono::system_clock::now();
        const auto sample_wall = injected.wall_now();
        const auto wall_after = std::chrono::system_clock::now();
        check(wall_before <= sample_wall && sample_wall <= wall_after,
              "system wall clock not bracketed by system_clock");
    }
    // Do NOT assert that wall time is monotonic: NTP and manual adjustments
    // can legitimately move the wall clock backwards or forwards.
}

} // namespace

int main() {
    try {
        test_type_contract();
        test_injected_fake_with_independent_time_domains();
        test_polymorphic_lifetime_and_separate_instances();
        test_real_clock_bracketing();
        std::cout << "PASS: Phase 5A injectable wall/monotonic clocks, fake jumps, "
                     "polymorphism and real source\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
