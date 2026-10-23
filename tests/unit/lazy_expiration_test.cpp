#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/commands/KeyCommands.hpp"
#include "redisdb/commands/StringCommands.hpp"
#include "redisdb/protocol/RespValue.hpp"
#include "redisdb/storage/Clock.hpp"
#include "redisdb/storage/Database.hpp"

#include <chrono>
#include <iostream>
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

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return tick_; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return wall_; }
    void advance(std::chrono::milliseconds delta) { wall_ += delta; }

  private:
    MonotonicTimePoint tick_ = MonotonicTimePoint{} + 10s;
    WallTimePoint wall_ = WallTimePoint{} + 1000s;
};

RespValue call(const CommandDispatcher& routes, std::string verb,
               std::vector<std::string> arguments) {
    std::vector<RespValue> fields;
    fields.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& argument : arguments) {
        fields.push_back(RespValue::bulk_string(std::move(argument)));
    }
    auto request = redisdb::commands::decode_command(RespValue::array(std::move(fields)));
    check(request.command.has_value(), "RESP request decoding failed");
    auto response = routes.dispatch(*request.command);
    check(response.handled.has_value(), "route is not registered");
    return std::move(response.handled->reply);
}

CommandDispatcher dispatcher(Database& db) {
    auto routes = redisdb::commands::string_routes(db);
    for (auto& route : redisdb::commands::key_routes(db)) {
        routes.push_back(std::move(route));
    }
    return CommandDispatcher(std::move(routes));
}

void test_exact_deadline_binary_keys_and_clock_rollback() {
    ManualClock fake;
    Database db(fake);
    auto routes = dispatcher(db);
    const std::string key("expire\0\xff", 8);
    check(call(routes, "SET", {key, "payload"}) == RespValue::simple_string("OK"),
          "SET binary fixture failed");
    check(db.set_expiration(key, fake.wall_now() + 50ms), "set deadline failed");
    fake.advance(49ms);
    check(call(routes, "GET", {key}) == RespValue::bulk_string("payload"),
          "early GET expired live binary key");
    check(call(routes, "TYPE", {key}) == RespValue::simple_string("string"),
          "early TYPE expired live binary key");
    fake.advance(1ms);
    check(call(routes, "GET", {key}) == RespValue::null_bulk_string(),
          "key visible at exact expiration instant");
    check(call(routes, "TYPE", {key}) == RespValue::simple_string("none"),
          "expired key TYPE not none");
    check(call(routes, "EXISTS", {key, key}) == RespValue::integer(0),
          "expired binary duplicate EXISTS count wrong");
    check(!db.expiration(key) && db.size() == 0, "GET did not physically purge expired entry");
    fake.advance(-70ms);
    check(!db.exists(key), "time rollback resurrected expired entry");
    check(db.set(key, "new"), "SET after expiry should create new entry");
    check(!db.expiration(key) && db.get(key) == "new", "SET after expiry kept old deadline");
}

void test_multi_key_atomic_snapshot_and_lazy_only_cleanup() {
    ManualClock fake;
    Database db(fake);
    auto routes = dispatcher(db);
    for (const auto& key : {"short", "long", "untouched"}) {
        check(call(routes, "SET", {key, key}) == RespValue::simple_string("OK"),
              "MGET fixture SET failed");
    }
    check(db.set_expiration("short", fake.wall_now() + 10ms), "short deadline failed");
    check(db.set_expiration("long", fake.wall_now() + 20ms), "long deadline failed");
    check(db.set_expiration("untouched", fake.wall_now() + 10ms), "untouched deadline failed");
    fake.advance(10ms);
    // No background expiration in Phase 5C; size() reports physical slots.
    check(db.size() == 3, "lazy-only semantics swept entries without access");
    check(call(routes, "MGET", {"short", "long", "short", "absent"}) ==
              RespValue::array({RespValue::null_bulk_string(), RespValue::bulk_string("long"),
                                RespValue::null_bulk_string(), RespValue::null_bulk_string()}),
          "MGET expired nil/duplicate/order mismatch");
    check(db.size() == 2, "MGET should purge accessed expired key only");
    check(call(routes, "EXISTS", {"long", "long", "short"}) == RespValue::integer(2),
          "EXISTS duplicate counting after lazy cleanup wrong");
    check(call(routes, "MSET", {"short", "replaced", "long", "new"}) ==
              RespValue::simple_string("OK"),
          "MSET with expired key failed");
    check(db.get("short") == "replaced" && db.get("long") == "new",
          "MSET did not produce fresh values");
    check(!db.expiration("short") && !db.expiration("long"),
          "MSET did not clear replaced deadlines");
    check(db.size() == 3, "MSET must not sweep unrelated expired entries");
    check(!db.exists("untouched") && db.size() == 2,
          "untouched expired key not removed on later access");
}

void test_expired_write_paths_and_rejections() {
    ManualClock fake;
    Database db(fake);
    auto routes = dispatcher(db);
    for (const auto& key : {"append", "integer", "deleted", "length", "meta"}) {
        check(call(routes, "SET", {key, "stale"}) == RespValue::simple_string("OK"),
              "write path fixture setup failed");
        check(db.set_expiration(key, fake.wall_now() + 5ms), "write path deadline failed");
    }
    fake.advance(5ms);
    check(call(routes, "APPEND", {"append", "OK"}) == RespValue::integer(2),
          "APPEND should start a fresh value after expiration");
    check(db.get("append") == "OK" && !db.expiration("append"),
          "APPEND reused expired bytes or metadata");
    check(call(routes, "INCR", {"integer"}) == RespValue::integer(1),
          "INCR should treat expired noninteger value as missing");
    check(db.get("integer") == "1" && !db.expiration("integer"),
          "INCR incorrectly preserved expired metadata");
    check(call(routes, "DEL", {"deleted", "deleted"}) == RespValue::integer(0),
          "DEL counted already-expired key");
    check(call(routes, "STRLEN", {"length"}) == RespValue::integer(0),
          "STRLEN exposed expired bytes");
    check(!db.set_expiration("meta", fake.wall_now() + 1s),
          "expired metadata mutation revived dead key");
    check(!db.clear_expiration("meta") && !db.expiration("meta"),
          "expired metadata API exposed dead key");

    check(call(routes, "SET", {"overflow", "9223372036854775807"}) ==
              RespValue::simple_string("OK"),
          "numeric fixture setup failed");
    check(db.set_expiration("overflow", fake.wall_now() + 4ms),
          "numeric expiration fixture failed");
    auto error = call(routes, "INCR", {"overflow"});
    check(error == RespValue::error("ERR increment or decrement would overflow"),
          "unexpired overflow was not rejected");
    check(db.get("overflow") == "9223372036854775807" && db.expiration("overflow"),
          "rejected numeric mutation destroyed value or metadata");
    fake.advance(4ms);
    check(call(routes, "INCR", {"overflow"}) == RespValue::integer(1),
          "expired overflow key did not restart counter from zero");

    check(call(routes, "SET", {"limit", "old"}) == RespValue::simple_string("OK"),
          "oversized fixture SET failed");
    check(db.set_expiration("limit", fake.wall_now() + 1ms), "oversized fixture deadline failed");
    fake.advance(1ms);
    const auto failed = call(routes, "APPEND", {"limit", std::string(8177, 'x')});
    check(failed == RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit"),
          "oversized APPEND should be an error");
    check(!db.exists("limit"), "oversized APPEND revived expired key");
    check(call(routes, "SET", {"limit", "ok"}) == RespValue::simple_string("OK"),
          "SET after rejected append failed");
    check(db.get("limit") == "ok" && !db.expiration("limit"),
          "SET after rejected append inherited stale deadline");
}

void test_direct_default_clock_and_past_deadline() {
    // Production default clock is wall time, not an intentionally disabled
    // expiration mode. An epoch deadline is expired on current machines.
    Database db;
    check(db.set("default", "alive"), "default SET failed");
    check(db.set_expiration("default", WallTimePoint{}), "default past deadline setup failed");
    check(!db.get("default") && db.size() == 0,
          "default SystemClock did not enforce an old wall deadline");
}
} // namespace

int main() {
    try {
        test_exact_deadline_binary_keys_and_clock_rollback();
        test_multi_key_atomic_snapshot_and_lazy_only_cleanup();
        test_expired_write_paths_and_rejections();
        test_direct_default_clock_and_past_deadline();
        std::cout << "PASS: Phase 5C exact lazy deadlines, binary keys, read/write/metadata "
                     "semantics, no eager sweep\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
