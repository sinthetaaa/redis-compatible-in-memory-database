#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/commands/KeyCommands.hpp"
#include "redisdb/commands/StringCommands.hpp"
#include "redisdb/protocol/RespValue.hpp"
#include "redisdb/storage/Clock.hpp"
#include "redisdb/storage/Database.hpp"

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using redisdb::commands::CommandDispatcher;
using redisdb::protocol::RespValue;
using redisdb::storage::Clock;
using redisdb::storage::Database;
using redisdb::storage::MonotonicTimePoint;
using redisdb::storage::WallTimePoint;

namespace {
using namespace std::chrono_literals;

void check(bool condition, const char* explanation) {
    if (!condition) {
        throw std::runtime_error(explanation);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return mono_; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return wall_; }
    void advance_wall(std::chrono::milliseconds value) { wall_ += value; }

  private:
    MonotonicTimePoint mono_ = MonotonicTimePoint{} + 10s;
    WallTimePoint wall_ = WallTimePoint{} + 80s;
};

RespValue call(const CommandDispatcher& dispatcher, std::string name,
               std::vector<std::string> arguments) {
    std::vector<RespValue> encoded;
    encoded.push_back(RespValue::bulk_string(std::move(name)));
    for (auto& arg : arguments) {
        encoded.push_back(RespValue::bulk_string(std::move(arg)));
    }
    auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(encoded)));
    check(decoded.command.has_value(), "metadata regression request decoding failed");
    auto outcome = dispatcher.dispatch(*decoded.command);
    check(outcome.handled.has_value(), "metadata regression command not registered");
    return std::move(outcome.handled->reply);
}

void test_deadlines_and_metadata_lifecycle() {
    Database database;
    auto routes = redisdb::commands::string_routes(database);
    for (auto& route : redisdb::commands::key_routes(database)) {
        routes.push_back(std::move(route));
    }
    const CommandDispatcher dispatcher(std::move(routes));
    ManualClock fake;
    const Clock& clock = fake;
    const auto first = clock.wall_now() + 3s;
    const auto second = clock.wall_now() + 9s;
    const std::string binary_key("x\0\xff", 3);

    check(!database.set_expiration("missing", first), "expiration setter created absent key");
    check(!database.clear_expiration("missing"), "clear on missing key returned success");
    check(!database.expiration("missing"), "missing key exposed expiration");
    check(database.size() == 0, "metadata access changed the keyspace");

    check(call(dispatcher, "SET", {binary_key, "7"}) == RespValue::simple_string("OK"),
          "binary key setup failed");
    check(!database.expiration(binary_key), "new key unexpectedly has a deadline");
    check(database.set_expiration(binary_key, first), "set deadline failed");
    check(database.expiration(binary_key) == first, "deadline metadata was not stored");
    check(database.set_expiration(binary_key, second), "deadline replacement failed");
    check(database.expiration(binary_key) == second, "deadline replacement not visible");

    // Even if wall time passes the deadline, Phase 5B MUST NOT expire keys.
    fake.advance_wall(1h);
    check(clock.wall_now() > second, "fake clock did not cross expiration deadline");
    check(call(dispatcher, "GET", {binary_key}) == RespValue::bulk_string("7"),
          "5B prematurely hid an expired key");
    check(call(dispatcher, "EXISTS", {binary_key}) == RespValue::integer(1),
          "5B prematurely removed expired-key existence");
    check(call(dispatcher, "TYPE", {binary_key}) == RespValue::simple_string("string"),
          "5B prematurely changed expired-key type");
    check(database.expiration(binary_key) == second, "GET changed stored expiration");

    check(call(dispatcher, "APPEND", {binary_key, "0"}) == RespValue::integer(2),
          "append mutation failed");
    check(database.expiration(binary_key) == second, "APPEND dropped expiration metadata");
    check(call(dispatcher, "INCR", {binary_key}) == RespValue::integer(71),
          "numeric mutation failed");
    check(database.expiration(binary_key) == second, "INCR dropped expiration metadata");
    check(call(dispatcher, "DECRBY", {binary_key, "1"}) == RespValue::integer(70),
          "numeric decrement failed");
    check(database.expiration(binary_key) == second, "DECRBY dropped expiration metadata");

    auto error = call(dispatcher, "INCRBY", {binary_key, "not-numeric"});
    check(std::holds_alternative<redisdb::protocol::Error>(error.storage()),
          "invalid operand did not return error");
    check(database.get(binary_key) == "70" && database.expiration(binary_key) == second,
          "rejected arithmetic modified value or deadline");

    check(call(dispatcher, "SET", {binary_key, "new"}) == RespValue::simple_string("OK"),
          "SET overwrite failed");
    check(!database.expiration(binary_key), "SET should clear existing expiration metadata");
    check(!database.clear_expiration(binary_key), "clearing absent deadline returned success");
    check(database.set_expiration(binary_key, first), "reassigning deadline failed");
    check(database.clear_expiration(binary_key), "explicit deadline clear failed");
    check(!database.clear_expiration(binary_key), "deadline clear was not idempotent");
    check(database.get(binary_key) == "new", "clearing deadline corrupted value");

    check(database.set_expiration(binary_key, first), "deadline reattach failed");
    check(call(dispatcher, "DEL", {binary_key}) == RespValue::integer(1), "DEL failed");
    check(!database.expiration(binary_key), "deleted key retained expiration metadata");
    check(call(dispatcher, "SET", {binary_key, "reborn"}) == RespValue::simple_string("OK"),
          "key reinsert failed");
    check(!database.expiration(binary_key), "reinserted key inherited old expiration");
}

void test_batch_replacement_and_preserved_deadlines() {
    Database database;
    auto routes = redisdb::commands::string_routes(database);
    const CommandDispatcher dispatcher(std::move(routes));
    const auto first = WallTimePoint{} + 55s;
    const auto second = WallTimePoint{} - 3s;
    const std::string binary_key("\0\xff", 2);

    for (const auto& key : {"a", "b", "untouched"}) {
        check(call(dispatcher, "SET", {key, "1"}) == RespValue::simple_string("OK"),
              "batch setup failed");
        check(database.set_expiration(key, first), "batch deadline initialization failed");
    }
    check(call(dispatcher, "MSET", {"a", "fresh", "b", "interim", "b", "final"}) ==
              RespValue::simple_string("OK"),
          "MSET failed");
    check(database.get("a") == "fresh" && database.get("b") == "final",
          "MSET ordering or values wrong");
    check(!database.expiration("a") && !database.expiration("b"),
          "MSET did not clear touched key deadlines");
    check(database.expiration("untouched") == first, "MSET changed untouched key metadata");

    check(call(dispatcher, "APPEND", {binary_key, "abc"}) == RespValue::integer(3),
          "missing-key APPEND failed");
    check(!database.expiration(binary_key), "new APPEND key has unexpected deadline");
    check(database.set_expiration(binary_key, second), "binary deadline set failed");
    check(call(dispatcher, "APPEND", {binary_key, ""}) == RespValue::integer(3),
          "empty-suffix APPEND failed");
    check(database.expiration(binary_key) == second, "empty APPEND modified metadata");
    std::string overlarge(8177, 'x');
    auto rejected = call(dispatcher, "APPEND", {binary_key, overlarge});
    check(std::holds_alternative<redisdb::protocol::Error>(rejected.storage()),
          "oversize APPEND not rejected");
    check(database.get(binary_key) == "abc" && database.expiration(binary_key) == second,
          "rejected APPEND mutated value/deadline");

    check(call(dispatcher, "INCRBY", {"new-counter", "5"}) == RespValue::integer(5),
          "missing-key INCRBY failed");
    check(!database.expiration("new-counter"), "new integer acquired expiration");
    check(database.set_expiration("new-counter", first), "counter deadline setup failed");
    check(call(dispatcher, "INCRBY", {"new-counter", "9223372036854775807"}).storage().index() == 1,
          "overflow counter was accepted");
    check(database.get("new-counter") == "5" && database.expiration("new-counter") == first,
          "overflow changed counter/deadline");

    database.clear();
    check(database.size() == 0 && !database.expiration("untouched") &&
              !database.expiration(binary_key),
          "clear did not erase deadline metadata");
}
} // namespace

int main() {
    try {
        test_deadlines_and_metadata_lifecycle();
        test_batch_replacement_and_preserved_deadlines();
        std::cout
            << "PASS: Phase 5B wall-deadline metadata, atomic lifecycle, no early expiration\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
