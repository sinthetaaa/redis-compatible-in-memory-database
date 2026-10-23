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

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return now_; }
    void advance(std::chrono::milliseconds amount) { now_ += amount; }
    void set_wall(WallTimePoint point) { now_ = point; }

  private:
    WallTimePoint now_ = WallTimePoint{} + 1000s;
};

CommandDispatcher routes(Database& db) {
    auto handlers = redisdb::commands::string_routes(db);
    for (auto& route : redisdb::commands::key_routes(db)) {
        handlers.push_back(std::move(route));
    }
    return CommandDispatcher(std::move(handlers));
}

RespValue call(const CommandDispatcher& d, std::string cmd, std::vector<std::string> args) {
    std::vector<RespValue> elems;
    elems.push_back(RespValue::bulk_string(std::move(cmd)));
    for (auto& arg : args) {
        elems.push_back(RespValue::bulk_string(std::move(arg)));
    }
    const auto request = redisdb::commands::decode_command(RespValue::array(std::move(elems)));
    check(request.command.has_value(), "RESP2 decode failed");
    auto result = d.dispatch(*request.command);
    check(result.handled.has_value(), "command not registered");
    return std::move(result.handled->reply);
}

void test_seconds_and_milliseconds() {
    ManualClock clock;
    Database db(clock);
    auto d = routes(db);
    const std::string binary("k\0\xff", 3);
    check(call(d, "EXPIRE", {binary, "10"}) == RespValue::integer(0), "missing EXPIRE");
    check(call(d, "PEXPIRE", {binary, "10"}) == RespValue::integer(0), "missing PEXPIRE");
    check(call(d, "SET", {binary, "abc"}) == RespValue::simple_string("OK"), "binary setup");
    check(call(d, "eXpIrE", {binary, "2"}) == RespValue::integer(1), "EXPIRE mixed case");
    check(db.expiration(binary) == clock.wall_now() + 2s, "seconds deadline incorrect");
    clock.advance(1999ms);
    check(call(d, "GET", {binary}) == RespValue::bulk_string("abc"), "seconds early expiration");
    clock.advance(1ms);
    check(call(d, "GET", {binary}) == RespValue::null_bulk_string(), "seconds exact expiry");
    check(call(d, "EXPIRE", {binary, "0"}) == RespValue::integer(0), "already expired EXPIRE");
    check(call(d, "SET", {binary, "v"}) == RespValue::simple_string("OK"), "reset after expiry");
    check(call(d, "pExPiRe", {binary, "150"}) == RespValue::integer(1), "PEXPIRE mixed case");
    check(db.expiration(binary) == clock.wall_now() + 150ms, "milliseconds deadline incorrect");
    clock.advance(149ms);
    check(db.get(binary) == "v", "milliseconds early expiration");
    clock.advance(1ms);
    check(!db.exists(binary), "milliseconds exact expiration");
    clock.advance(-1h);
    check(!db.exists(binary), "clock rollback resurrected expired key");
}

void test_immediate_expiry_and_replacements() {
    ManualClock clock;
    Database db(clock);
    auto d = routes(db);
    check(call(d, "SET", {"x", "1"}) == RespValue::simple_string("OK"), "seed x");
    check(call(d, "EXPIRE", {"x", "0"}) == RespValue::integer(1), "zero seconds delete");
    check(call(d, "TYPE", {"x"}) == RespValue::simple_string("none"), "zero delete absent");
    check(call(d, "SET", {"x", "2"}) == RespValue::simple_string("OK"), "reset x");
    check(call(d, "PEXPIRE", {"x", "-9223372036854775808"}) == RespValue::integer(1),
          "minimum signed millis delete");
    check(!db.exists("x"), "negative delete retained key");
    check(call(d, "SET", {"counter", "10"}) == RespValue::simple_string("OK"), "counter seed");
    check(call(d, "EXPIRE", {"counter", "8"}) == RespValue::integer(1), "counter deadline");
    check(call(d, "INCR", {"counter"}) == RespValue::integer(11), "counter INCR");
    check(db.expiration("counter") == clock.wall_now() + 8s, "INCR lost timeout");
    check(call(d, "PEXPIRE", {"counter", "2000"}) == RespValue::integer(1), "timeout override");
    check(db.expiration("counter") == clock.wall_now() + 2s, "timeout not replaced");
    check(call(d, "APPEND", {"counter", "2"}) == RespValue::integer(3), "append after expire");
    check(db.expiration("counter") == clock.wall_now() + 2s, "APPEND changed timeout");
    check(call(d, "SET", {"counter", "fresh"}) == RespValue::simple_string("OK"), "set override");
    check(!db.expiration("counter"), "SET did not clear timeout");
    check(call(d, "EXPIRE", {"counter", "-3"}) == RespValue::integer(1), "negative sec delete");
    check(call(d, "GET", {"counter"}) == RespValue::null_bulk_string(), "negative sec not absent");
}

void test_rejected_arguments_preserve_value_and_deadline() {
    ManualClock clock;
    Database db(clock);
    auto d = routes(db);
    check(call(d, "SET", {"x", "42"}) == RespValue::simple_string("OK"), "reject setup");
    check(call(d, "PEXPIRE", {"x", "10000"}) == RespValue::integer(1), "reject deadline setup");
    const auto deadline = db.expiration("x");
    const auto invalid = RespValue::error("ERR value is not an integer or out of range");
    for (const auto& text : {"", "+1", "01", "-0", " 1", "1 ", "1z", "1.5", "9223372036854775808",
                             "-9223372036854775809"}) {
        check(call(d, "EXPIRE", {"x", text}) == invalid, "EXPIRE malformed duration accepted");
        check(call(d, "PEXPIRE", {"x", text}) == invalid, "PEXPIRE malformed duration accepted");
    }
    check(call(d, "EXPIRE", {}) ==
              RespValue::error("ERR wrong number of arguments for 'expire' command"),
          "EXPIRE arity");
    check(call(d, "PEXPIRE", {"x"}) ==
              RespValue::error("ERR wrong number of arguments for 'pexpire' command"),
          "PEXPIRE arity");
    check(call(d, "EXPIRE", {"x", "1", "NX"}) ==
              RespValue::error("ERR wrong number of arguments for 'expire' command"),
          "unplanned NX option");
    check(call(d, "EXPIRE", {"x", "9223372036854775807"}) ==
              RespValue::error("ERR invalid expire time in 'expire' command"),
          "seconds overflow");
    check(call(d, "PEXPIRE", {"x", "9223372036854775807"}) ==
              RespValue::error("ERR invalid expire time in 'pexpire' command"),
          "millis overflow");
    check(db.get("x") == "42" && db.expiration("x") == deadline,
          "invalid expiry mutated existing value or deadline");
    clock.set_wall(WallTimePoint{
        WallTimePoint::duration{std::numeric_limits<WallTimePoint::duration::rep>::max() - 10}});
    check(call(d, "SET", {"near-max", "safe"}) == RespValue::simple_string("OK"), "near-max setup");
    check(call(d, "PEXPIRE", {"near-max", "1"}) ==
              RespValue::error("ERR invalid expire time in 'pexpire' command"),
          "near-limit wall+duration overflow");
    check(db.get("near-max") == "safe" && !db.expiration("near-max"),
          "near-max failed expiry mutated key");
}

void test_expired_key_and_empty_binary_argument() {
    ManualClock clock;
    Database db(clock);
    auto d = routes(db);
    check(call(d, "SET", {"", "empty"}) == RespValue::simple_string("OK"), "empty-key seed");
    check(call(d, "PEXPIRE", {"", "1"}) == RespValue::integer(1), "empty key PEXPIRE");
    clock.advance(1ms);
    check(call(d, "EXPIRE", {"", "5"}) == RespValue::integer(0), "expired empty-key revival");
    check(call(d, "TYPE", {""}) == RespValue::simple_string("none"), "expired empty key TYPE");
    check(call(d, "SET", {"", "new"}) == RespValue::simple_string("OK"), "empty recreate");
    check(call(d, "PEXPIRE", {"", "-1"}) == RespValue::integer(1), "empty delete");
    check(!db.exists(""), "empty key deletion retained key");
}

} // namespace
int main() {
    try {
        test_seconds_and_milliseconds();
        test_immediate_expiry_and_replacements();
        test_rejected_arguments_preserve_value_and_deadline();
        test_expired_key_and_empty_binary_argument();
        std::cout << "PASS: Phase 5D EXPIRE/PEXPIRE strict parsing, immediate deletion, exact "
                     "deadlines, overflow rollback\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
