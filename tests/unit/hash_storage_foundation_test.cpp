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
#include <unordered_map>
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
void check(bool ok, const char* note) {
    if (!ok) {
        throw std::runtime_error(note);
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

template <typename Callback> void expect_wrong_type(Callback&& callback, const char* label) {
    bool caught = false;
    try {
        (void)callback();
    } catch (const WrongTypeError&) {
        caught = true;
    }
    check(caught, label);
}

CommandDispatcher make_dispatcher(Database& db) {
    auto routes = redisdb::commands::string_routes(db);
    for (auto& route : redisdb::commands::key_routes(db)) {
        routes.push_back(std::move(route));
    }
    return CommandDispatcher(std::move(routes));
}

RespValue call(const CommandDispatcher& dispatcher, std::string name,
               std::vector<std::string> arguments) {
    std::vector<RespValue> request;
    request.push_back(RespValue::bulk_string(std::move(name)));
    for (auto& arg : arguments) {
        request.push_back(RespValue::bulk_string(std::move(arg)));
    }
    auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(request)));
    check(decoded.command.has_value(), "command decode");
    const auto outcome = dispatcher.dispatch(*decoded.command);
    check(outcome.handled.has_value(), "command is registered");
    return outcome.handled->reply;
}

void test_missing_binary_duplicate_and_owned_copy() {
    Database db;
    const std::string key("hash\0\xff", 6);
    const std::string field("f\0\x80", 3);
    const std::string bytes("v\0\xff\r\n", 5);
    check(db.type_of(key) == StoredType::None, "missing hash type");
    check(db.hash_length(key) == 0 && !db.hash_get(key, "x") && !db.hash_exists(key, "x") &&
              db.hash_delete(key, {"x"}) == 0,
          "missing hash reads and delete");
    check(db.hash_items(key, 8192).has_value() && db.hash_items(key, 8192)->empty(),
          "missing HGETALL");
    bool invalid = false;
    try {
        (void)db.hash_set(key, {});
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    check(invalid && !db.exists(key), "empty HSET created a key");
    invalid = false;
    try {
        (void)db.hash_delete(key, {});
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    check(invalid && !db.exists(key), "empty HDEL changed storage");
    check(db.hash_set(key, {{field, "v1"}, {"other", ""}, {field, bytes}}) == 2,
          "HSET duplicate fields count only once");
    check(db.type_of(key) == StoredType::Hash && db.hash_length(key) == 2, "TYPE and count");
    check(db.hash_get(key, field) == bytes && db.hash_get(key, "other") == "" &&
              db.hash_exists(key, field) && !db.hash_exists(key, "absent"),
          "binary-safe field and payload");
    check(db.hash_set(key, {{field, "new"}, {"added", "v"}, {"added", "updated"}}) == 1,
          "HSET new and replaced fields");
    check(db.hash_get(key, field) == "new" && db.hash_get(key, "added") == "updated" &&
              db.hash_length(key) == 3,
          "HSET last-wins and cardinality");
    auto snapshot = db.hash_items(key, 8192);
    check(snapshot.has_value() && snapshot->size() == 3, "HGETALL snapshot");
    std::unordered_map<std::string, std::string> content(snapshot->begin(), snapshot->end());
    check(content.at(field) == "new" && content.at("other") == "" &&
              content.at("added") == "updated",
          "unordered HGETALL values");
    snapshot->front().second = "changed";
    check(db.hash_get(key, field) == "new", "snapshot must own independent bytes");
    check(db.hash_delete(key, {"missing", field, field}) == 1 && !db.hash_get(key, field) &&
              db.hash_length(key) == 2,
          "HDEL duplicate count and remaining fields");
    check(db.hash_delete(key, {"other", "added"}) == 2 && db.type_of(key) == StoredType::None &&
              db.size() == 0,
          "last HDEL must remove empty hash key");
}

void test_wrongtype_and_string_interoperability() {
    Database db;
    const auto dispatcher = make_dispatcher(db);
    check(db.set("s", "9"), "seed string");
    check(db.list_push("list", {"x"}, false) == 1, "seed list");
    for (const auto& key : {"s", "list"}) {
        expect_wrong_type([&] { return db.hash_set(key, {{"a", "x"}}); }, "wrong hash_set");
        expect_wrong_type([&] { return db.hash_get(key, "a"); }, "wrong hash_get");
        expect_wrong_type([&] { return db.hash_exists(key, "a"); }, "wrong hash_exists");
        expect_wrong_type([&] { return db.hash_length(key); }, "wrong hash_length");
        expect_wrong_type([&] { return db.hash_delete(key, {"a"}); }, "wrong hash_delete");
        expect_wrong_type([&] { return db.hash_items(key, 8192); }, "wrong hash_items");
    }
    check(db.get("s") == "9" && db.list_length("list") == 1, "wrongtype mutation");
    check(db.hash_set("hash", {{"f", "v"}}) == 1, "seed hash");
    const auto wrong =
        RespValue::error("WRONGTYPE Operation against a key holding the wrong kind of value");
    check(call(dispatcher, "TYPE", {"hash"}) == RespValue::simple_string("hash"), "TYPE hash");
    check(call(dispatcher, "GET", {"hash"}) == wrong &&
              call(dispatcher, "APPEND", {"hash", "x"}) == wrong &&
              call(dispatcher, "STRLEN", {"hash"}) == wrong &&
              call(dispatcher, "INCR", {"hash"}) == wrong,
          "string routes reject hash");
    check(call(dispatcher, "MGET", {"s", "hash", "missing"}) ==
              RespValue::array({RespValue::bulk_string("9"), RespValue::null_bulk_string(),
                                RespValue::null_bulk_string()}),
          "MGET non-string is nil");
    check(call(dispatcher, "SET", {"hash", "text"}) == RespValue::simple_string("OK") &&
              db.type_of("hash") == StoredType::String && db.get("hash") == "text",
          "SET replaces hash");
    check(db.hash_set("other", {{"a", "1"}}) == 1, "seed second hash");
    check(call(dispatcher, "MSET", {"other", "10"}) == RespValue::simple_string("OK") &&
              db.get("other") == "10",
          "MSET replaces hash");
}

void test_ttl_cleanup_and_budget() {
    ManualClock clock;
    Database db(clock);
    check(db.hash_set("timed", {{"a", "1"}, {"b", "2"}}) == 2, "seed TTL hash");
    const auto deadline = clock.wall_now() + 100ms;
    check(db.set_expiration("timed", deadline), "set TTL");
    check(db.hash_set("timed", {{"a", "replaced"}, {"c", "3"}}) == 1 &&
              db.expiration("timed") == deadline,
          "HSET must preserve TTL");
    check(db.hash_delete("timed", {"a", "b"}) == 2 && db.expiration("timed") == deadline &&
              db.hash_length("timed") == 1,
          "HDEL non-last preserves TTL");
    check(db.hash_delete("timed", {"c"}) == 1 && !db.exists("timed") && !db.expiration("timed"),
          "HDEL last removes TTL");
    clock.advance(100ms);
    check(db.sweep_expired(32).inspected == 0, "orphaned TTL record after HDEL");
    check(db.hash_set("expires", {{"x", "y"}}) == 1, "new TTL hash");
    check(db.set_expiration("expires", clock.wall_now() + 10ms), "expire deadline");
    clock.advance(11ms);
    check(!db.hash_get("expires", "x") && db.type_of("expires") == StoredType::None &&
              db.hash_length("expires") == 0,
          "lazy expiry must hide hash");
    check(db.sweep_expired(32).inspected == 0, "lazy expiry must remove index");
    check(db.hash_set("many", {{"a", std::string(4090, 'a')}, {"b", std::string(4090, 'b')}}) == 2,
          "large hash seed");
    check(!db.hash_items("many", 8192).has_value() && db.hash_length("many") == 2,
          "bounded HGETALL must be nonmutating");
    check(db.hash_items("many", 10000).has_value(), "higher reply budget");
    check(!db.hash_items("missing", 3).has_value(), "too small budget");
    check(db.hash_items("missing", 4).has_value(), "empty array four-byte budget");
}
} // namespace

int main() {
    try {
        test_missing_binary_duplicate_and_owned_copy();
        test_wrongtype_and_string_interoperability();
        test_ttl_cleanup_and_budget();
        std::cout << "PASS: Phase 6C.1 binary-safe hash storage and TTL/type safety\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
