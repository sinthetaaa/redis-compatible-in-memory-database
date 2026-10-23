#include "redisdb/storage/Clock.hpp"
#include "redisdb/storage/Database.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using redisdb::storage::Clock;
using redisdb::storage::Database;
using redisdb::storage::MonotonicTimePoint;
using redisdb::storage::SetWithExpiryResult;
using redisdb::storage::WallTimePoint;

void require(bool ok, const char* why) {
    if (!ok) {
        throw std::runtime_error(why);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return now_; }
    void advance(std::chrono::milliseconds duration) { now_ += duration; }

  private:
    WallTimePoint now_{WallTimePoint{} + 1000s};
};

void test_budget_exact_deadline_and_persistent_keys() {
    ManualClock clock;
    Database db(clock);
    require(db.set_with_expiration("a", "A", 100, true) == SetWithExpiryResult::Set, "timed key a");
    require(db.set_with_expiration("b", "B", 100, true) == SetWithExpiryResult::Set, "timed key b");
    require(db.set_with_expiration("c", "C", 100, true) == SetWithExpiryResult::Set, "timed key c");
    require(db.set("permanent", "P"), "persistent key");
    require(db.size() == 4, "initial physical count");
    auto r = db.sweep_expired(0);
    require(r.inspected == 0 && r.deleted == 0, "zero budget deleted keys");
    clock.advance(99ms);
    r = db.sweep_expired(2);
    require(r.inspected == 0 && r.deleted == 0, "expired early");
    clock.advance(1ms);
    r = db.sweep_expired(2);
    require(r.inspected == 2 && r.deleted == 2 && db.size() == 2,
            "budget exceeded or exact expiry missed");
    r = db.sweep_expired(2);
    require(r.inspected == 1 && r.deleted == 1 && db.size() == 1,
            "due key not picked up in second turn");
    r = db.sweep_expired(32);
    require(r.inspected == 0 && r.deleted == 0, "stale index after sweep");
    require(db.get("permanent") == "P", "persistent key was evicted");
}

void test_deadline_changes_and_key_lifecycle() {
    ManualClock clock;
    Database db(clock);
    const std::string key("binary\0\xff", 8);
    require(db.set_with_expiration(key, "old", 10, true) == SetWithExpiryResult::Set,
            "initial binary key");
    require(db.set_with_expiration(key, "new", 500, true) == SetWithExpiryResult::Set,
            "replaced old deadline");
    require(db.set_with_expiration("other", "value", 10, true) == SetWithExpiryResult::Set,
            "other timed key");
    clock.advance(10ms);
    auto r = db.sweep_expired(10);
    require(r.deleted == 1 && db.size() == 1 && db.get(key) == "new",
            "old deadline incorrectly expired updated key");
    require(db.clear_expiration(key), "PERSIST equivalent failed");
    clock.advance(500ms);
    r = db.sweep_expired(10);
    require(r.deleted == 0 && db.get(key) == "new", "PERSIST did not clear index");
    require(db.set_expiration(key, clock.wall_now() + 1ms), "set expiration metadata");
    require(!db.set(key, "plain"), "plain SET should replace existing key");
    clock.advance(1ms);
    r = db.sweep_expired(10);
    require(r.deleted == 0 && db.get(key) == "plain", "plain SET retained expiration index");
    require(db.set_expiration(key, clock.wall_now()), "attach already-due metadata");
    r = db.sweep_expired(1);
    require(r.deleted == 1 && !db.exists(key), "due metadata not removed proactively");
    require(db.size() == 0, "deleted key left physical state");
}

void test_mset_erase_lazy_clear_and_failed_write() {
    ManualClock clock;
    Database db(clock);
    require(db.set_with_expiration("a", "old", 20, true) == SetWithExpiryResult::Set, "seed a");
    require(db.set_with_expiration("b", "old", 20, true) == SetWithExpiryResult::Set, "seed b");
    require(db.set_with_expiration("c", "old", 20, true) == SetWithExpiryResult::Set, "seed c");
    db.set_many({{"a", "new"}, {"b", "new"}, {"a", "winner"}});
    require(db.erase("c"), "DEL equivalent failed");
    require(db.get("a") == "winner", "MSET last-wins broke");
    require(db.set_with_expiration("d", "kept", 50, true) == SetWithExpiryResult::Set, "seed d");
    require(db.set_with_expiration("d", "bad", std::numeric_limits<std::int64_t>::max(), false) ==
                SetWithExpiryResult::OutOfRange,
            "overflowing timeout accepted");
    require(db.get("d") == "kept", "rejected SET changed value");
    clock.advance(20ms);
    auto r = db.sweep_expired(32);
    require(r.deleted == 0 && db.get("a") == "winner" && db.get("b") == "new",
            "MSET/DEL left stale scheduled deletes");
    require(db.set_expiration("b", clock.wall_now() + 1ms), "new deadline after MSET");
    clock.advance(1ms);
    require(!db.exists("b"), "lazy expiry did not erase b");
    r = db.sweep_expired(32);
    require(r.deleted == 0, "lazy removal left a scheduled duplicate");
    db.clear();
    clock.advance(1s);
    r = db.sweep_expired(32);
    require(r.inspected == 0 && r.deleted == 0 && db.size() == 0,
            "CLEAR did not reset active expiration index");
}

void test_large_due_backlog_bounded_turns_and_clock_rollback() {
    ManualClock clock;
    Database db(clock);
    for (int i = 0; i < 97; ++i) {
        require(db.set_with_expiration("due:" + std::to_string(i), "x", 1, true) ==
                    SetWithExpiryResult::Set,
                "seed many timed keys");
    }
    require(db.set_with_expiration("future", "F", 5, false) == SetWithExpiryResult::Set,
            "future key");
    clock.advance(-10ms);
    auto r = db.sweep_expired(7);
    require(r.inspected == 0 && r.deleted == 0, "wall clock rollback expired keys early");
    clock.advance(11ms);
    std::size_t removed = 0;
    for (int i = 0; i < 30; ++i) {
        r = db.sweep_expired(7);
        require(r.inspected <= 7 && r.deleted <= 7, "unbounded expiration maintenance batch");
        removed += r.deleted;
    }
    require(removed == 97 && db.size() == 1, "backlog did not converge fairly");
    require(db.get("future") == "F", "live future deadline removed");
    r = db.sweep_expired(7);
    require(r.inspected == 0 && r.deleted == 0, "future index record looked due");
    clock.advance(5s);
    r = db.sweep_expired(7);
    require(r.deleted == 1 && db.size() == 0, "future key not removed after deadline");
}

} // namespace

int main() {
    try {
        test_budget_exact_deadline_and_persistent_keys();
        test_deadline_changes_and_key_lifecycle();
        test_mset_erase_lazy_clear_and_failed_write();
        test_large_due_backlog_bounded_turns_and_clock_rollback();
        std::cout << "PASS: Phase 5G bounded indexed sweeps, mutation lifecycle, rollback, "
                     "MSET atomicity, binary keys, 97-key backlog and fake clocks\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
