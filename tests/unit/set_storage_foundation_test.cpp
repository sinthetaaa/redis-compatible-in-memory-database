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
#include <unordered_set>
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

template <typename Callback> void wrong_type(Callback&& callback, const char* label) {
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
    check(outcome.handled.has_value(), "registered command");
    return outcome.handled->reply;
}

void test_missing_binary_uniqueness_and_owned_snapshot() {
    Database db;
    const std::string key("s\0\xff", 3);
    const std::string member("m\0\x80", 3);
    check(db.type_of(key) == StoredType::None, "missing set type");
    check(!db.set_contains(key, member) && db.set_cardinality(key) == 0 &&
              db.set_remove(key, {member}) == 0,
          "missing key basic set operations");
    auto empty = db.set_members(key, 4);
    check(empty.has_value() && empty->empty(), "missing SMEMBERS empty array");
    bool invalid = false;
    try {
        (void)db.set_add(key, {});
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    check(invalid && !db.exists(key), "empty SADD cannot create key");
    invalid = false;
    try {
        (void)db.set_remove(key, {});
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    check(invalid && !db.exists(key), "empty SREM cannot mutate key");
    check(db.set_add(key, {member, "", member, "z"}) == 3, "SADD repeated members count only once");
    check(db.type_of(key) == StoredType::Set && db.set_cardinality(key) == 3,
          "set type and cardinality");
    check(db.set_contains(key, member) && db.set_contains(key, "") && !db.set_contains(key, "M") &&
              !db.set_contains(key, "missing"),
          "byte-exact membership");
    check(db.set_add(key, {"z", member}) == 0 && db.set_cardinality(key) == 3,
          "SADD existing members no change");
    check(db.set_add(key, {"new", "new", "z"}) == 1 && db.set_cardinality(key) == 4,
          "SADD mixed duplicate/existing/new");
    auto members = db.set_members(key, 8192);
    check(members.has_value() && members->size() == 4, "SMEMBERS snapshot exists");
    const std::unordered_set<std::string> present(members->begin(), members->end());
    check(present.size() == 4 && present.contains(member) && present.contains("") &&
              present.contains("z") && present.contains("new"),
          "unordered member set content");
    members->front() = "mutated";
    check(!db.set_contains(key, "mutated"), "snapshot is independently owned");
    check(db.set_remove(key, {"absent", member, member}) == 1 && !db.set_contains(key, member) &&
              db.set_cardinality(key) == 3,
          "SREM repeated members only counted once");
    check(db.set_remove(key, {"", "z", "new"}) == 3 && db.size() == 0 &&
              db.type_of(key) == StoredType::None,
          "last member removal removes set key");
}

void test_wrongtype_and_interoperation() {
    Database db;
    const auto dispatcher = make_dispatcher(db);
    check(db.set("string", "7"), "seed string");
    check(db.list_push("list", {"x"}, false) == 1, "seed list");
    check(db.hash_set("hash", {{"a", "x"}}) == 1, "seed hash");
    for (const auto& key : {"string", "list", "hash"}) {
        wrong_type([&] { return db.set_add(key, {"a"}); }, "set_add rejects wrongtype");
        wrong_type([&] { return db.set_remove(key, {"a"}); }, "set_remove rejects wrongtype");
        wrong_type([&] { return db.set_contains(key, "a"); }, "set_contains rejects wrongtype");
        wrong_type([&] { return db.set_cardinality(key); }, "set_cardinality rejects wrongtype");
        wrong_type([&] { return db.set_members(key, 8192); }, "set_members rejects wrongtype");
    }
    check(db.get("string") == "7" && db.list_length("list") == 1 && db.hash_length("hash") == 1,
          "no wrongtype mutation");
    check(db.set_add("set", {"a", "b"}) == 2, "create set");
    const auto error =
        RespValue::error("WRONGTYPE Operation against a key holding the wrong kind of value");
    check(call(dispatcher, "TYPE", {"set"}) == RespValue::simple_string("set"), "TYPE set");
    check(call(dispatcher, "GET", {"set"}) == error &&
              call(dispatcher, "STRLEN", {"set"}) == error &&
              call(dispatcher, "APPEND", {"set", "x"}) == error &&
              call(dispatcher, "INCR", {"set"}) == error,
          "existing string commands reject set");
    check(call(dispatcher, "MGET", {"string", "set", "none"}) ==
              RespValue::array({RespValue::bulk_string("7"), RespValue::null_bulk_string(),
                                RespValue::null_bulk_string()}),
          "MGET yields nil for collection");
    check(call(dispatcher, "SET", {"set", "replacement"}) == RespValue::simple_string("OK") &&
              db.get("set") == "replacement" && db.type_of("set") == StoredType::String,
          "SET replaces set");
    check(db.set_add("other", {"z"}) == 1, "make second set");
    check(call(dispatcher, "MSET", {"other", "new"}) == RespValue::simple_string("OK") &&
              db.get("other") == "new",
          "MSET replaces set");
}

void test_ttl_expiration_and_reply_bounds() {
    ManualClock clock;
    Database db(clock);
    check(db.set_add("timed", {"a", "b"}) == 2, "seed timed set");
    const auto deadline = clock.wall_now() + 100ms;
    check(db.set_expiration("timed", deadline), "install deadline");
    check(db.set_add("timed", {"a", "c"}) == 1 && db.expiration("timed") == deadline,
          "set add retains TTL");
    check(db.set_remove("timed", {"a", "b"}) == 2 && db.set_cardinality("timed") == 1 &&
              db.expiration("timed") == deadline,
          "nonfinal set remove retains TTL");
    check(db.set_remove("timed", {"c"}) == 1 && !db.exists("timed") && !db.expiration("timed"),
          "final member clears expiration index");
    clock.advance(101ms);
    check(db.sweep_expired(32).inspected == 0, "no orphaned expiry index");
    check(db.set_add("expires", {"x"}) == 1, "seed expiring set");
    check(db.set_expiration("expires", clock.wall_now() + 10ms), "set expiry");
    clock.advance(11ms);
    check(!db.set_contains("expires", "x") && db.set_cardinality("expires") == 0 &&
              db.type_of("expires") == StoredType::None,
          "lazy expiry hides entire set");
    check(db.sweep_expired(32).inspected == 0, "lazy expiry removes index");
    check(db.set_add("large", {std::string(4090, 'a'), std::string(4090, 'b')}) == 2,
          "large set members accepted at storage layer");
    check(!db.set_members("large", 8192).has_value() && db.set_cardinality("large") == 2,
          "SMEMBERS budget nonmutating");
    check(db.set_members("large", 10000).has_value(), "larger budget succeeds");
    check(!db.set_members("absent", 3).has_value(), "invalid zero-element reply budget");
    check(db.set_members("absent", 4).has_value(), "four-byte empty reply");
    check(db.set_add("single", {std::string(8176, 'x')}) == 1, "single safe member");
    check(db.set_members("single", 8192).has_value(), "single max-size member reply");
    check(!db.set_members("single", 8188).has_value(), "one byte below encoded size");
}
} // namespace

int main() {
    try {
        test_missing_binary_uniqueness_and_owned_snapshot();
        test_wrongtype_and_interoperation();
        test_ttl_expiration_and_reply_bounds();
        std::cout << "PASS: Phase 6D.1A storage set uniqueness, TTL, WRONGTYPE and bounds\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
