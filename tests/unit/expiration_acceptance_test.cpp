#include "redisdb/storage/Clock.hpp"
#include "redisdb/storage/Database.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using redisdb::storage::Clock;
using redisdb::storage::Database;
using redisdb::storage::ExpireResult;
using redisdb::storage::MonotonicTimePoint;
using redisdb::storage::SetWithExpiryResult;
using redisdb::storage::WallTimePoint;

void require(bool ok, const std::string& why) {
    if (!ok) {
        throw std::runtime_error(why);
    }
}

class ManualClock final : public Clock {
  public:
    std::int64_t milliseconds_now = 1'000'000;
    [[nodiscard]] WallTimePoint wall_now() const noexcept override {
        return WallTimePoint{} + std::chrono::milliseconds{milliseconds_now};
    }
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
    void advance(std::int64_t delta) { milliseconds_now += delta; }
};

class Random final {
  public:
    explicit Random(std::uint64_t seed) : state_(seed) {}
    [[nodiscard]] std::uint64_t next() {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return state_;
    }
    [[nodiscard]] std::size_t choose(std::size_t n) { return static_cast<std::size_t>(next() % n); }

  private:
    std::uint64_t state_;
};

struct ModelEntry {
    std::string value;
    std::optional<std::int64_t> deadline_ms;
};
using Model = std::unordered_map<std::string, ModelEntry>;

void model_expire_key(Model& model, const std::string& key, std::int64_t now) {
    const auto it = model.find(key);
    if (it != model.end() && it->second.deadline_ms.has_value() && *it->second.deadline_ms <= now) {
        model.erase(it);
    }
}

[[nodiscard]] std::int64_t model_ttl(const Model& model, const std::string& key, std::int64_t now,
                                     bool milliseconds) {
    const auto it = model.find(key);
    if (it == model.end()) {
        return -2;
    }
    if (!it->second.deadline_ms.has_value()) {
        return -1;
    }
    const auto remaining = *it->second.deadline_ms - now;
    require(remaining > 0, "oracle stale key in TTL query");
    return milliseconds ? remaining : (remaining + 500) / 1000;
}

void check_key(Database& database, Model& model, const std::string& key, std::int64_t now,
               const std::string& context) {
    model_expire_key(model, key, now);
    const auto expected = model.find(key);
    const auto got = database.get(key);
    require(got.has_value() == (expected != model.end()), context + ": GET existence mismatch");
    if (expected != model.end()) {
        require(*got == expected->second.value, context + ": GET value mismatch");
    }
    require(database.exists(key) == (expected != model.end()), context + ": EXISTS mismatch");
    require(database.remaining_ttl(key, true) == model_ttl(model, key, now, true),
            context + ": PTTL mismatch");
    require(database.remaining_ttl(key, false) == model_ttl(model, key, now, false),
            context + ": TTL mismatch");
}

void test_fixed_seed_reference_model() {
    ManualClock clock;
    Database database(clock);
    Model model;
    Random random(0x5A17E5C0FFEE1234ULL);
    std::vector<std::string> keys;
    keys.reserve(64);
    keys.emplace_back("");
    keys.emplace_back(std::string("\0\xff\r\n", 4));
    for (int i = 2; i < 64; ++i) {
        keys.push_back("exp:" + std::to_string(i));
    }
    constexpr std::int64_t Duration[] = {1, 2, 7, 19, 200, 499, 500, 1000, 1501, 5000};
    for (std::size_t step = 0; step < 4096; ++step) {
        if (step % 5 == 0) {
            const auto tick = static_cast<std::int64_t>(random.choose(93));
            clock.advance(random.choose(13) == 0 ? -tick : tick);
        }
        const std::string& key = keys[random.choose(keys.size())];
        const std::string value = "v" + std::to_string(step);
        const auto duration = Duration[random.choose(sizeof(Duration) / sizeof(Duration[0]))];
        const auto now = clock.milliseconds_now;
        const auto action = random.choose(12);
        const auto context = "step " + std::to_string(step) + " action " + std::to_string(action);
        if (action == 0) {
            model_expire_key(model, key, now);
            const bool should_insert = model.find(key) == model.end();
            require(database.set(key, value) == should_insert, context + ": SET result");
            model[key] = ModelEntry{value, std::nullopt};
        } else if (action == 1) {
            require(database.set_with_expiration(key, value, duration, true) ==
                        SetWithExpiryResult::Set,
                    context + ": SET PX result");
            model[key] = ModelEntry{value, now + duration};
        } else if (action == 2) {
            model_expire_key(model, key, now);
            const bool present = model.find(key) != model.end();
            const auto result = database.expire_after(key, duration, true);
            require(result == (present ? ExpireResult::Set : ExpireResult::Missing),
                    context + ": PEXPIRE result");
            if (present) {
                model[key].deadline_ms = now + duration;
            }
        } else if (action == 3) {
            model_expire_key(model, key, now);
            const bool present = model.erase(key) != 0;
            const auto result = database.expire_after(key, 0, false);
            require(result == (present ? ExpireResult::Deleted : ExpireResult::Missing),
                    context + ": EXPIRE zero result");
        } else if (action == 4) {
            model_expire_key(model, key, now);
            const auto found = model.find(key);
            const bool previously_timed =
                found != model.end() && found->second.deadline_ms.has_value();
            require(database.clear_expiration(key) == previously_timed,
                    context + ": PERSIST result");
            if (found != model.end()) {
                found->second.deadline_ms.reset();
            }
        } else if (action == 5) {
            model_expire_key(model, key, now);
            const bool was_present = model.erase(key) != 0;
            require(database.erase(key) == was_present, context + ": DEL result");
        } else if (action == 6) {
            model_expire_key(model, key, now);
            const std::string suffix = ":a";
            const auto length = database.append(key, suffix, 8176);
            require(length.has_value(), context + ": APPEND rejected bounded value");
            if (model.find(key) == model.end()) {
                model[key] = ModelEntry{suffix, std::nullopt};
            } else {
                model[key].value.append(suffix);
            }
            require(*length == model[key].value.size(), context + ": APPEND length");
        } else if (action == 7) {
            const std::string& second = keys[random.choose(keys.size())];
            database.set_many({{key, value}, {second, "changed"}, {key, "final"}});
            model[key] = ModelEntry{"final", std::nullopt};
            if (second != key) {
                model[second] = ModelEntry{"changed", std::nullopt};
            }
        } else if (action == 8) {
            check_key(database, model, key, now, context);
        } else if (action == 9) {
            model_expire_key(model, key, now);
            require(database.exists(key) == (model.find(key) != model.end()),
                    context + ": EXISTS result");
        } else if (action == 10) {
            model_expire_key(model, key, now);
            require(database.remaining_ttl(key, true) == model_ttl(model, key, now, true),
                    context + ": PTTL result");
            require(database.remaining_ttl(key, false) == model_ttl(model, key, now, false),
                    context + ": TTL result");
        } else {
            const std::size_t budget = random.choose(9);
            // Model physical sweeps as well as lazy access. Without this,
            // rolling the wall clock backward could incorrectly resurrect an
            // oracle entry already removed by an earlier active sweep.
            std::vector<std::pair<std::int64_t, std::string>> due;
            for (const auto& [candidate, entry] : model) {
                if (entry.deadline_ms.has_value() && *entry.deadline_ms <= now) {
                    due.emplace_back(*entry.deadline_ms, candidate);
                }
            }
            std::sort(due.begin(), due.end());
            const auto expected = std::min(budget, due.size());
            for (std::size_t index = 0; index < expected; ++index) {
                model.erase(due[index].second);
            }
            const auto result = database.sweep_expired(budget);
            require(result.inspected == expected && result.deleted == expected,
                    context + ": deadline-ordered sweep disagrees with oracle");
        }
        check_key(database, model, keys[random.choose(keys.size())], now, context + " probe");
    }
    for (const auto& key : keys) {
        check_key(database, model, key, clock.milliseconds_now, "final census");
    }
    require(database.size() == model.size(), "complete logical/physical census differs");
    const auto empty = database.sweep_expired(64);
    require(empty.deleted == 0, "sweep deleted live key after complete census");
}

void test_513_key_bounded_convergence() {
    ManualClock clock;
    Database database(clock);
    for (std::size_t i = 0; i < 513; ++i) {
        const auto key = "due:" + std::to_string(i);
        require(database.set_with_expiration(key, "x", 1, true) == SetWithExpiryResult::Set,
                "backlog timed SET");
    }
    require(database.set("persistent", "P"), "persistent backlog sentinel");
    require(database.set_with_expiration("future", "F", 5000, true) == SetWithExpiryResult::Set,
            "future backlog sentinel");
    require(database.size() == 515, "backlog physical size");
    require(database.sweep_expired(0).deleted == 0, "zero-budget sweep removed key");
    clock.advance(-2);
    require(database.sweep_expired(32).inspected == 0, "wall rollback expired early");
    clock.advance(3);
    std::size_t all_deleted = 0;
    std::size_t turns = 0;
    while (all_deleted != 513 && turns < 20) {
        const auto batch = database.sweep_expired(32);
        require(batch.inspected <= 32 && batch.deleted <= batch.inspected,
                "unbounded active maintenance turn");
        all_deleted += batch.deleted;
        ++turns;
    }
    require(all_deleted == 513 && turns == 17, "513-key backlog not bounded and convergent");
    require(database.size() == 2, "sweep removed sentinel or left expired physical key");
    require(database.get("persistent") == "P" && database.get("future") == "F",
            "sweep corrupted live sentinel");
    require(database.sweep_expired(32).inspected == 0, "future key treated as due");
    clock.advance(4999);
    require(database.sweep_expired(32).deleted == 1, "future key not removed on exact deadline");
    require(database.get("persistent") == "P" && database.size() == 1,
            "persistent sentinel not retained");
    database.clear();
    require(database.sweep_expired(32).inspected == 0 && database.size() == 0,
            "CLEAR left active index entries");
}

void test_rejected_expirations_preserve_state() {
    ManualClock clock;
    Database database(clock);
    require(database.set_with_expiration("guard", "original", 1000, true) ==
                SetWithExpiryResult::Set,
            "guard seed");
    const auto original_deadline = database.expiration("guard");
    require(database.set_with_expiration("guard", "changed", 0, true) ==
                SetWithExpiryResult::OutOfRange,
            "SET PX zero accepted");
    require(database.set_with_expiration("guard", "changed",
                                         std::numeric_limits<std::int64_t>::max(),
                                         false) == SetWithExpiryResult::OutOfRange,
            "SET EX overflow accepted");
    require(database.expire_after("guard", std::numeric_limits<std::int64_t>::max(), false) ==
                ExpireResult::OutOfRange,
            "EXPIRE overflow accepted");
    require(database.get("guard") == "original" &&
                database.expiration("guard") == original_deadline,
            "failed timeout corrupted live value or deadline");
    clock.advance(1000);
    require(database.sweep_expired(32).deleted == 1 && !database.exists("guard"),
            "rejected writes incorrectly rescheduled guard key");
}
} // namespace

int main() {
    try {
        test_fixed_seed_reference_model();
        test_513_key_bounded_convergence();
        test_rejected_expirations_preserve_state();
        std::cout << "PASS: Phase 5H 4096-step deterministic reference model, "
                     "513-key 32-budget backlog, overflow rollback and exact deadline checks\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
