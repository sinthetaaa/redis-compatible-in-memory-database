#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/commands/KeyCommands.hpp"
#include "redisdb/commands/StringCommands.hpp"
#include "redisdb/protocol/RespValue.hpp"
#include "redisdb/storage/Clock.hpp"
#include "redisdb/storage/Database.hpp"

#include <chrono>
#include <cstddef>
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
using redisdb::storage::StoredType;
using redisdb::storage::WallTimePoint;
using redisdb::storage::WrongTypeError;

namespace {
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return now_; }
    void advance(std::chrono::milliseconds delta) { now_ += delta; }

  private:
    WallTimePoint now_{WallTimePoint{} + 1000s};
};

RespValue call(const CommandDispatcher& dispatcher, std::string name,
               std::vector<std::string> args) {
    std::vector<RespValue> request;
    request.push_back(RespValue::bulk_string(std::move(name)));
    for (auto& arg : args) {
        request.push_back(RespValue::bulk_string(std::move(arg)));
    }
    auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(request)));
    check(decoded.command.has_value(), "command decoding failed");
    auto outcome = dispatcher.dispatch(*decoded.command);
    check(outcome.handled.has_value(), "command not registered");
    return std::move(outcome.handled->reply);
}

CommandDispatcher string_key_dispatcher(Database& database) {
    auto routes = redisdb::commands::string_routes(database);
    for (auto& route : redisdb::commands::key_routes(database)) {
        routes.push_back(std::move(route));
    }
    return CommandDispatcher(std::move(routes));
}

template <typename Callback> void expect_wrong_type(Callback&& callback, const char* message) {
    bool caught = false;
    try {
        (void)callback();
    } catch (const WrongTypeError& e) {
        caught = std::string(e.what()) ==
                 "WRONGTYPE Operation against a key holding the wrong kind of value";
    }
    check(caught, message);
}

void test_push_pop_binary_order_and_missing() {
    Database db;
    const std::string key("key\0\xff", 5);
    const std::string binary("A\0\x80\xff", 4);
    check(db.type_of(key) == StoredType::None, "missing type");
    check(db.list_length(key) == 0 && !db.list_pop(key, true) && !db.list_pop(key, false),
          "missing list operations");
    bool invalid = false;
    try {
        (void)db.list_push(key, {}, true);
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    check(invalid && !db.exists(key), "empty list push created key");
    check(db.list_push(key, {"first", binary, "first"}, false) == 3, "RPUSH count");
    check(db.list_push(key, {"front1", "front2"}, true) == 5, "LPUSH count");
    check(db.type_of(key) == StoredType::List && db.list_length(key) == 5, "list type/count");
    check(db.list_pop(key, true) == "front2", "LPUSH argument ordering");
    check(db.list_pop(key, false) == "first", "RPUSH last item");
    check(db.list_pop(key, true) == "front1", "left pop ordering");
    check(db.list_pop(key, true) == "first", "duplicate preservation");
    check(db.list_pop(key, false) == binary, "binary-safe owned pop");
    check(db.type_of(key) == StoredType::None && !db.exists(key) && db.size() == 0,
          "last pop failed to remove physical key");
}

void test_type_rejection_and_old_string_commands() {
    Database db;
    const auto dispatcher = string_key_dispatcher(db);
    const RespValue wrong =
        RespValue::error("WRONGTYPE Operation against a key holding the wrong kind of value");
    check(call(dispatcher, "SET", {"s", "42"}) == RespValue::simple_string("OK"), "SET seed");
    expect_wrong_type([&] { return db.list_push("s", {"x"}, true); }, "LPUSH on string");
    expect_wrong_type([&] { return db.list_length("s"); }, "LLEN on string");
    expect_wrong_type([&] { return db.list_pop("s", false); }, "RPOP on string");
    check(db.get("s") == "42" && db.type_of("s") == StoredType::String,
          "wrong type operation mutated string");

    check(db.list_push("list", {"item"}, false) == 1, "direct list creation");
    check(call(dispatcher, "TYPE", {"list"}) == RespValue::simple_string("list"),
          "TYPE must report list");
    check(call(dispatcher, "EXISTS", {"list", "list"}) == RespValue::integer(2),
          "EXISTS must count repeated list key");
    expect_wrong_type([&] { return db.get("list"); }, "direct GET must reject list");
    expect_wrong_type([&] { return db.append("list", "suffix", 8176); },
                      "direct APPEND must reject list");
    expect_wrong_type([&] { return db.string_length("list"); }, "direct STRLEN must reject list");
    expect_wrong_type([&] { return db.adjust_integer("list", 1, false); },
                      "direct INCR must reject list");
    check(call(dispatcher, "GET", {"list"}) == wrong, "GET must reply WRONGTYPE");
    check(call(dispatcher, "APPEND", {"list", "x"}) == wrong, "APPEND must reply WRONGTYPE");
    check(call(dispatcher, "STRLEN", {"list"}) == wrong, "STRLEN must reply WRONGTYPE");
    check(call(dispatcher, "INCR", {"list"}) == wrong, "INCR must reply WRONGTYPE");
    check(call(dispatcher, "DECR", {"list"}) == wrong, "DECR must reply WRONGTYPE");
    check(call(dispatcher, "INCRBY", {"list", "2"}) == wrong, "INCRBY must reply WRONGTYPE");
    check(call(dispatcher, "DECRBY", {"list", "2"}) == wrong, "DECRBY must reply WRONGTYPE");
    check(call(dispatcher, "MGET", {"list", "s", "absent", "list"}) ==
              RespValue::array({RespValue::null_bulk_string(), RespValue::bulk_string("42"),
                                RespValue::null_bulk_string(), RespValue::null_bulk_string()}),
          "MGET must return nil for non-string and absent keys");
    check(db.list_length("list") == 1 && db.list_pop("list", true) == "item",
          "failed string access changed live list");
}

void test_list_ttl_and_cross_type_replace() {
    ManualClock clock;
    Database db(clock);
    const auto dispatcher = string_key_dispatcher(db);
    check(db.list_push("timed", {"a", "b"}, false) == 2, "timed list creation");
    const auto deadline = clock.wall_now() + 250ms;
    check(db.set_expiration("timed", deadline), "list TTL set");
    check(db.list_push("timed", {"c"}, true) == 3, "push to timed list");
    check(db.expiration("timed") == deadline && db.list_pop("timed", false) == "b",
          "list push lost TTL or pop failed");
    check(db.expiration("timed") == deadline, "list pop lost TTL");
    clock.advance(249ms);
    check(db.type_of("timed") == StoredType::List && db.list_length("timed") == 2,
          "list expired too early");
    clock.advance(1ms);
    check(db.type_of("timed") == StoredType::None && db.list_length("timed") == 0,
          "list failed to expire at deadline");
    check(db.sweep_expired(32).inspected == 0 && db.size() == 0,
          "expired list left stale deadline index");

    check(db.list_push("last", {"only"}, true) == 1, "last element setup");
    check(db.set_expiration("last", clock.wall_now() + 500ms), "last element TTL setup");
    check(db.list_pop("last", true) == "only" && !db.exists("last"),
          "last element did not delete list key");
    clock.advance(500ms);
    check(db.sweep_expired(32).inspected == 0, "last pop retained expiration index");

    check(db.list_push("replace", {"one", "two"}, true) == 2, "list before SET");
    check(db.set_expiration("replace", clock.wall_now() + 5s), "SET replacement TTL setup");
    check(call(dispatcher, "SET", {"replace", "new"}) == RespValue::simple_string("OK"),
          "SET must overwrite list");
    check(db.type_of("replace") == StoredType::String && db.get("replace") == "new" &&
              !db.expiration("replace"),
          "SET did not clear list TTL");
    check(db.list_push("replace2", {"x"}, false) == 1, "list before MSET");
    check(db.set_expiration("replace2", clock.wall_now() + 5s), "MSET list TTL setup");
    check(call(dispatcher, "MSET", {"replace2", "10", "other", "ok"}) ==
              RespValue::simple_string("OK"),
          "MSET failed to replace list");
    check(db.type_of("replace2") == StoredType::String && db.get("replace2") == "10" &&
              !db.expiration("replace2") && db.get("other") == "ok",
          "MSET failed atomic list replacement");
    check(db.list_push("del", {"x"}, false) == 1, "list before DEL");
    check(call(dispatcher, "DEL", {"del", "del"}) == RespValue::integer(1) && !db.exists("del"),
          "DEL list should count once");
}

} // namespace

int main() {
    try {
        test_push_pop_binary_order_and_missing();
        test_type_rejection_and_old_string_commands();
        test_list_ttl_and_cross_type_replace();
        std::cout << "PASS: list storage and safe cross-type command routing\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
