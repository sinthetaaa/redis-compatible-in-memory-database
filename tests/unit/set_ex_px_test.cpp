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

void check(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return current_; }
    void advance(std::chrono::milliseconds delta) { current_ += delta; }
    void set_wall(WallTimePoint now) { current_ = now; }

  private:
    WallTimePoint current_ = WallTimePoint{} + 1000s;
};

CommandDispatcher dispatcher(Database& db) {
    auto routes = redisdb::commands::string_routes(db);
    for (auto& route : redisdb::commands::key_routes(db)) {
        routes.push_back(std::move(route));
    }
    return CommandDispatcher(std::move(routes));
}

RespValue call(const CommandDispatcher& d, std::string verb, std::vector<std::string> args) {
    std::vector<RespValue> values;
    values.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& argument : args) {
        values.push_back(RespValue::bulk_string(std::move(argument)));
    }
    const auto request = redisdb::commands::decode_command(RespValue::array(std::move(values)));
    check(request.command.has_value(), "invalid unit command request");
    auto outcome = d.dispatch(*request.command);
    check(outcome.handled.has_value(), "SET route missing");
    return std::move(outcome.handled->reply);
}

void test_binary_key_and_exact_deadlines() {
    ManualClock clock;
    Database db(clock);
    auto d = dispatcher(db);
    const std::string key("e\0\xff\r\n", 5);
    const std::string data("v\0\xfe", 3);
    const auto ok = RespValue::simple_string("OK");
    check(call(d, "SeT", {key, data, "eX", "2"}) == ok, "SET EX binary failed");
    check(call(d, "GET", {key}) == RespValue::bulk_string(data), "binary value changed");
    check(call(d, "PTTL", {key}) == RespValue::integer(2000), "EX deadline wrong");
    clock.advance(1999ms);
    check(db.get(key) == data, "EX expired early");
    check(call(d, "PTTL", {key}) == RespValue::integer(1), "EX last ms missing");
    clock.advance(1ms);
    check(call(d, "TTL", {key}) == RespValue::integer(-2), "EX exact deadline failed");
    check(call(d, "SET", {key, "next", "pX", "151"}) == ok, "PX replacement failed");
    check(call(d, "PTTL", {key}) == RespValue::integer(151), "PX deadline wrong");
    clock.advance(150ms);
    check(db.get(key) == "next", "PX expired prematurely");
    clock.advance(1ms);
    check(call(d, "GET", {key}) == RespValue::null_bulk_string(), "PX exact deadline failed");
    clock.advance(-5s);
    check(!db.exists(key), "rollback resurrected physically expired key");
}

void test_replacement_and_preservation() {
    ManualClock clock;
    Database db(clock);
    auto d = dispatcher(db);
    const auto ok = RespValue::simple_string("OK");
    check(call(d, "SET", {"k", "old", "EX", "10"}) == ok, "first SET EX");
    check(call(d, "SET", {"k", "updated", "PX", "1500"}) == ok, "new PX overwrote EX");
    check(db.get("k") == "updated", "value not replaced");
    check(call(d, "PTTL", {"k"}) == RespValue::integer(1500), "old deadline not replaced");
    check(call(d, "INCRBY", {"n", "5"}) == RespValue::integer(5), "counter seed");
    check(call(d, "SET", {"n", "10", "EX", "12"}) == ok, "counter timed replacement");
    check(call(d, "INCR", {"n"}) == RespValue::integer(11), "increment on timed key");
    check(call(d, "PTTL", {"n"}) == RespValue::integer(12000), "INCR lost deadline");
    check(call(d, "APPEND", {"n", "2"}) == RespValue::integer(3), "timed append");
    check(call(d, "PTTL", {"n"}) == RespValue::integer(12000), "APPEND lost deadline");
    check(call(d, "PERSIST", {"n"}) == RespValue::integer(1), "PERSIST timed key");
    check(call(d, "TTL", {"n"}) == RespValue::integer(-1), "PERSIST failed");
    check(call(d, "SET", {"k", "permanent"}) == ok, "plain SET on timed key");
    check(call(d, "PTTL", {"k"}) == RespValue::integer(-1), "plain SET retained TTL");
    check(db.get("k") == "permanent", "plain SET value changed");

    // No read should be needed between deadline passing and a new SET.
    check(call(d, "SET", {"stale", "old", "PX", "1"}) == ok, "stale seed");
    clock.advance(1ms);
    check(call(d, "SET", {"stale", "fresh", "EX", "4"}) == ok,
          "SET EX failed to replace physically retained expired key");
    check(db.get("stale") == "fresh", "stale bytes escaped replacement");
    check(call(d, "PTTL", {"stale"}) == RespValue::integer(4000),
          "stale deadline inherited instead of replacing");
}

void test_rejected_commands_are_read_only() {
    ManualClock clock;
    Database db(clock);
    auto d = dispatcher(db);
    const auto ok = RespValue::simple_string("OK");
    check(call(d, "SET", {"live", "original", "PX", "5000"}) == ok, "seed live");
    const auto deadline = db.expiration("live");
    const auto invalid = RespValue::error("ERR value is not an integer or out of range");
    const auto expiry = RespValue::error("ERR invalid expire time in 'set' command");
    const auto arity = RespValue::error("ERR wrong number of arguments for 'set' command");
    for (const auto* text : {"", "+1", "01", "-0", " 1", "1 ", "1.5", "1x", "9223372036854775808",
                             "-9223372036854775809"}) {
        check(call(d, "SET", {"live", "wrong", "EX", text}) == invalid,
              "malformed EX duration accepted");
        check(call(d, "SET", {"live", "wrong", "PX", text}) == invalid,
              "malformed PX duration accepted");
    }
    for (const auto* value : {"0", "-1", "-9223372036854775808", "9223372036854775807"}) {
        check(call(d, "SET", {"live", "bad", "EX", value}) == expiry,
              "invalid or overflowing EX accepted");
        check(call(d, "SET", {"live", "bad", "PX", value}) == expiry,
              "invalid or overflowing PX accepted");
    }
    for (const auto* option : {"NX", "XX", "GET", "XX\0", "EXTRA", ""}) {
        check(call(d, "SET", {"live", "bad", option, "1"}) == RespValue::error("ERR syntax error"),
              "unsupported option accepted");
    }
    check(call(d, "SET", {"live"}) == arity, "too few SET args");
    check(call(d, "SET", {"live", "bad", "EX"}) == arity, "missing EX amount");
    check(call(d, "SET", {"live", "bad", "EX", "2", "NX"}) == arity,
          "extra option silently accepted");
    check(call(d, "SET", {"live", std::string(8177, 'a'), "EX", "10"}) ==
              RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit"),
          "oversized SET EX accepted");
    check(db.get("live") == "original" && db.expiration("live") == deadline,
          "rejected SET mutated value or deadline");
    check(call(d, "SET", {"missing", "bad", "EX", "0"}) == expiry,
          "invalid SET EX created missing key");
    check(!db.exists("missing"), "rejected SET created key");

    // Test deadline+now overflow independently of multiplication overflow.
    clock.set_wall(WallTimePoint{
        WallTimePoint::duration{std::numeric_limits<WallTimePoint::duration::rep>::max() - 10}});
    check(call(d, "SET", {"near-max", "safe"}) == ok, "near-max seed");
    check(call(d, "SET", {"near-max", "bad", "PX", "1"}) == expiry,
          "near-max deadline overflow was accepted");
    check(db.get("near-max") == "safe" && !db.expiration("near-max"),
          "near-max failed mutation damaged key");
}

void test_boundary_value_size_and_empty_key() {
    ManualClock clock;
    Database db(clock);
    auto d = dispatcher(db);
    const auto ok = RespValue::simple_string("OK");
    check(call(d, "SET", {"", "", "PX", "2"}) == ok, "empty key/value timed SET");
    check(db.get("") == "", "empty binary string lost");
    check(call(d, "PTTL", {""}) == RespValue::integer(2), "empty key missing TTL");
    clock.advance(2ms);
    check(call(d, "GET", {""}) == RespValue::null_bulk_string(), "empty key expiry failed");
    const std::string limit(8176, 'X');
    check(call(d, "SET", {"limit", limit, "EX", "5"}) == ok, "limit size rejected");
    check(db.get("limit") == limit, "limit sized value corrupted");
    check(call(d, "SET", {"limit", limit + "x", "PX", "50"}) ==
              RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit"),
          "oversized replacement accepted");
    check(call(d, "PTTL", {"limit"}) == RespValue::integer(5000), "oversize lost TTL");
}
} // namespace

int main() {
    try {
        test_binary_key_and_exact_deadlines();
        test_replacement_and_preservation();
        test_rejected_commands_are_read_only();
        test_boundary_value_size_and_empty_key();
        std::cout << "PASS: Phase 5F atomic SET EX/PX, exact deadlines, strict options, "
                     "rollback and binary keys\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
