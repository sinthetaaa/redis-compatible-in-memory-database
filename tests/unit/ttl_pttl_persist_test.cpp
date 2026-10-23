#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/commands/KeyCommands.hpp"
#include "redisdb/commands/StringCommands.hpp"
#include "redisdb/protocol/RespValue.hpp"
#include "redisdb/storage/Clock.hpp"
#include "redisdb/storage/Database.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using redisdb::commands::CommandDispatcher;
using redisdb::protocol::RespValue;
using redisdb::storage::Clock;
using redisdb::storage::Database;
using redisdb::storage::MonotonicTimePoint;
using redisdb::storage::WallTimePoint;

namespace {
using namespace std::chrono_literals;

void check(bool result, const char* message) {
    if (!result) {
        throw std::runtime_error(message);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return wall_; }
    void advance(std::chrono::milliseconds value) { wall_ += value; }
    void set(WallTimePoint point) { wall_ = point; }

  private:
    WallTimePoint wall_ = WallTimePoint{} + 1000s;
};

CommandDispatcher make_dispatcher(Database& db) {
    auto routes = redisdb::commands::string_routes(db);
    for (auto& route : redisdb::commands::key_routes(db)) {
        routes.push_back(std::move(route));
    }
    return CommandDispatcher(std::move(routes));
}

RespValue call(const CommandDispatcher& d, std::string verb, std::vector<std::string> args) {
    std::vector<RespValue> parts;
    parts.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& argument : args) {
        parts.push_back(RespValue::bulk_string(std::move(argument)));
    }
    const auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(parts)));
    check(decoded.command.has_value(), "RESP request decoding failed");
    auto response = d.dispatch(*decoded.command);
    check(response.handled.has_value(), "missing TTL/PTTL/PERSIST route");
    return std::move(response.handled->reply);
}

void test_missing_persistent_and_binary_keys() {
    ManualClock fake;
    Database db(fake);
    auto d = make_dispatcher(db);
    const std::string key("t\0\xff", 3);
    check(call(d, "TTL", {key}) == RespValue::integer(-2), "missing TTL");
    check(call(d, "PTTL", {key}) == RespValue::integer(-2), "missing PTTL");
    check(call(d, "PERSIST", {key}) == RespValue::integer(0), "missing PERSIST");
    check(call(d, "SET", {key, "bytes"}) == RespValue::simple_string("OK"), "binary SET");
    check(call(d, "tTl", {key}) == RespValue::integer(-1), "persistent TTL");
    check(call(d, "pTtL", {key}) == RespValue::integer(-1), "persistent PTTL");
    check(call(d, "pErSiSt", {key}) == RespValue::integer(0), "persistent PERSIST");
    check(call(d, "PEXPIRE", {key, "2000"}) == RespValue::integer(1), "PEXPIRE seed");
    check(call(d, "PERSIST", {key}) == RespValue::integer(1), "PERSIST should remove deadline");
    check(call(d, "PERSIST", {key}) == RespValue::integer(0), "PERSIST idempotence");
    fake.advance(10s);
    check(db.get(key) == "bytes" && !db.expiration(key), "PERSIST damaged value or retained TTL");
    check(call(d, "TTL", {key}) == RespValue::integer(-1), "PERSIST future visibility");
}

void test_seconds_rounding_and_exact_deadline() {
    ManualClock fake;
    Database db(fake);
    auto d = make_dispatcher(db);
    check(call(d, "SET", {"clock", "v"}) == RespValue::simple_string("OK"), "clock seed");
    check(call(d, "PEXPIRE", {"clock", "2000"}) == RespValue::integer(1), "clock expiration");
    check(call(d, "TTL", {"clock"}) == RespValue::integer(2), "initial 2-second TTL");
    check(call(d, "PTTL", {"clock"}) == RespValue::integer(2000), "initial PTTL");
    fake.advance(499ms);
    check(call(d, "TTL", {"clock"}) == RespValue::integer(2), "1501ms round to 2");
    check(call(d, "PTTL", {"clock"}) == RespValue::integer(1501), "1501ms PTTL");
    fake.advance(1ms);
    check(call(d, "TTL", {"clock"}) == RespValue::integer(2), "exact half-up 1500ms");
    check(call(d, "PTTL", {"clock"}) == RespValue::integer(1500), "1500ms PTTL");
    fake.advance(1ms);
    check(call(d, "TTL", {"clock"}) == RespValue::integer(1), "1499ms round to 1");
    check(call(d, "PTTL", {"clock"}) == RespValue::integer(1499), "1499ms PTTL");
    fake.advance(999ms);
    check(call(d, "TTL", {"clock"}) == RespValue::integer(1), "exact half-up 500ms");
    fake.advance(1ms);
    check(call(d, "TTL", {"clock"}) == RespValue::integer(0), "499ms round to zero");
    check(call(d, "PTTL", {"clock"}) == RespValue::integer(499), "499ms PTTL");
    fake.advance(498ms);
    check(call(d, "PTTL", {"clock"}) == RespValue::integer(1), "last millisecond PTTL");
    check(db.get("clock") == "v", "live key vanished before deadline");
    fake.advance(1ms);
    check(call(d, "TTL", {"clock"}) == RespValue::integer(-2), "deadline TTL not missing");
    check(call(d, "PTTL", {"clock"}) == RespValue::integer(-2), "deadline PTTL not missing");
    check(call(d, "PERSIST", {"clock"}) == RespValue::integer(0),
          "expired PERSIST resurrected key");
    fake.advance(-10s);
    check(call(d, "TTL", {"clock"}) == RespValue::integer(-2), "clock rollback revived key");
}

void test_expiration_preservation_and_arguments() {
    ManualClock fake;
    Database db(fake);
    auto d = make_dispatcher(db);
    check(call(d, "SET", {"", "9"}) == RespValue::simple_string("OK"), "empty key setup");
    check(call(d, "EXPIRE", {"", "5"}) == RespValue::integer(1), "empty key EXPIRE");
    check(call(d, "INCR", {""}) == RespValue::integer(10), "INCR on volatile key");
    check(call(d, "PTTL", {""}) == RespValue::integer(5000), "INCR lost expiration");
    check(call(d, "APPEND", {"", "a"}) == RespValue::integer(3), "APPEND on volatile key");
    check(call(d, "TTL", {""}) == RespValue::integer(5), "APPEND lost expiration");
    check(call(d, "PERSIST", {""}) == RespValue::integer(1), "PERSIST after mutations");
    check(call(d, "GET", {""}) == RespValue::bulk_string("10a"), "PERSIST altered bytes");
    check(call(d, "TTL", {""}) == RespValue::integer(-1), "empty key not persistent");
    check(call(d, "EXPIRE", {"", "0"}) == RespValue::integer(1), "zero EXPIRE failed");
    check(call(d, "TTL", {""}) == RespValue::integer(-2), "immediate expiry TTL");
    check(call(d, "TTL", {}) == RespValue::error("ERR wrong number of arguments for 'ttl' command"),
          "TTL arity");
    check(call(d, "PTTL", {"x", "y"}) ==
              RespValue::error("ERR wrong number of arguments for 'pttl' command"),
          "PTTL arity");
    check(call(d, "PERSIST", {}) ==
              RespValue::error("ERR wrong number of arguments for 'persist' command"),
          "PERSIST arity");
    check(call(d, "PERSIST", {"x", "y"}) ==
              RespValue::error("ERR wrong number of arguments for 'persist' command"),
          "PERSIST excess arity");
}

void test_submillisecond_and_epoch_extremes() {
    using Duration = WallTimePoint::duration;
    using Rep = Duration::rep;
    using Period = Duration::period;
    constexpr auto Min = std::numeric_limits<Rep>::min();
    constexpr auto Max = std::numeric_limits<Rep>::max();
    ManualClock fake;
    fake.set(WallTimePoint{Duration{Min + 1000}});
    Database db(fake);
    auto d = make_dispatcher(db);
    check(call(d, "SET", {"wide", "value"}) == RespValue::simple_string("OK"), "wide seed");
    check(db.set_expiration("wide", WallTimePoint{Duration{Max - 1000}}), "wide deadline setup");
    const auto ttl_ms = db.remaining_ttl("wide", true);
    const auto ttl_s = db.remaining_ttl("wide", false);
    check(ttl_ms > 0 && ttl_s > 0, "large negative-to-positive epoch span overflow");
    const auto delta =
        static_cast<std::uint64_t>(Max - 1000) - static_cast<std::uint64_t>(Min + 1000);
    check(ttl_ms == static_cast<std::int64_t>(delta / (Period::den / 1000)),
          "extreme PTTL lost precision or overflowed");
    check(call(d, "PERSIST", {"wide"}) == RespValue::integer(1), "wide PERSIST");
    check(db.get("wide") == "value", "wide PERSIST removed value");

    ManualClock fine;
    Database sub(fine);
    auto f = make_dispatcher(sub);
    check(call(f, "SET", {"tick", "1"}) == RespValue::simple_string("OK"), "tick seed");
    check(sub.set_expiration("tick", fine.wall_now() + Duration{1}), "sub-ms deadline");
    check(call(f, "PTTL", {"tick"}) == RespValue::integer(0), "positive sub-ms PTTL must be zero");
    check(call(f, "TTL", {"tick"}) == RespValue::integer(0), "positive sub-ms TTL must be zero");
    check(db.remaining_ttl("wide", true) == -1, "PERSIST did not make wide key persistent");
}
} // namespace

int main() {
    try {
        test_missing_persistent_and_binary_keys();
        test_seconds_rounding_and_exact_deadline();
        test_expiration_preservation_and_arguments();
        test_submillisecond_and_epoch_extremes();
        std::cout << "PASS: Phase 5E TTL/PTTL/PERSIST missing/persistent, half-up rounding, "
                     "exact deadline, epoch safety and binary keys\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
