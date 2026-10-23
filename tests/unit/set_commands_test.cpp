#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/commands/HashCommands.hpp"
#include "redisdb/commands/KeyCommands.hpp"
#include "redisdb/commands/ListCommands.hpp"
#include "redisdb/commands/SetCommands.hpp"
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

CommandDispatcher dispatcher_for(Database& db) {
    auto routes = redisdb::commands::string_routes(db);
    for (auto& r : redisdb::commands::key_routes(db)) {
        routes.push_back(std::move(r));
    }
    for (auto& r : redisdb::commands::list_routes(db)) {
        routes.push_back(std::move(r));
    }
    for (auto& r : redisdb::commands::hash_routes(db)) {
        routes.push_back(std::move(r));
    }
    for (auto& r : redisdb::commands::set_routes(db)) {
        routes.push_back(std::move(r));
    }
    return CommandDispatcher(std::move(routes));
}

RespValue call(const CommandDispatcher& dispatcher, std::string command,
               std::vector<std::string> args = {}) {
    std::vector<RespValue> arr;
    arr.push_back(RespValue::bulk_string(std::move(command)));
    for (auto& arg : args) {
        arr.push_back(RespValue::bulk_string(std::move(arg)));
    }
    const auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(arr)));
    check(decoded.command.has_value(), "command decode");
    auto result = dispatcher.dispatch(*decoded.command);
    check(result.handled.has_value(), "set route must be registered");
    return result.handled->reply;
}

std::unordered_set<std::string> unordered_elements(const RespValue& reply) {
    const auto* arr = std::get_if<redisdb::protocol::Array>(&reply.storage());
    check(arr != nullptr, "SMEMBERS must return array");
    std::unordered_set<std::string> values;
    for (const auto& elem : arr->elements) {
        const auto* v = std::get_if<redisdb::protocol::BulkString>(&elem.storage());
        check(v != nullptr, "SMEMBERS elements must be bulk strings");
        values.insert(v->bytes);
    }
    check(arr->elements.size() == values.size(), "SMEMBERS must not contain duplicate members");
    return values;
}

void test_binary_membership_and_unordered() {
    Database db;
    auto d = dispatcher_for(db);
    const std::string key("set\0\xff", 5);
    const std::string one("m\0\x80", 3);
    check(call(d, "SCARD", {key}) == RespValue::integer(0), "missing SCARD");
    check(call(d, "SISMEMBER", {key, one}) == RespValue::integer(0), "missing member");
    check(call(d, "SREM", {key, one}) == RespValue::integer(0), "missing SREM");
    check(unordered_elements(call(d, "SMEMBERS", {key})).empty(), "missing SMEMBERS");
    check(call(d, "SMISMEMBER", {key, one, "x"}) ==
              RespValue::array({RespValue::integer(0), RespValue::integer(0)}),
          "missing SMISMEMBER");
    check(call(d, "sAdD", {key, one, "", "ABC", one, "abc", ""}) == RespValue::integer(4),
          "SADD newly inserted unique bytes");
    check(call(d, "TYPE", {key}) == RespValue::simple_string("set"), "TYPE set");
    check(call(d, "SCARD", {key}) == RespValue::integer(4), "SCARD 4");
    check(call(d, "SISMEMBER", {key, one}) == RespValue::integer(1), "binary ISMEMBER");
    check(call(d, "SISMEMBER", {key, "Abc"}) == RespValue::integer(0), "case-sensitive member");
    check(call(d, "SMISMEMBER", {key, "abc", "absent", one, "abc", ""}) ==
              RespValue::array({RespValue::integer(1), RespValue::integer(0), RespValue::integer(1),
                                RespValue::integer(1), RespValue::integer(1)}),
          "SMISMEMBER preserves request positions/repeats");
    check(unordered_elements(call(d, "SMEMBERS", {key})) ==
              (std::unordered_set<std::string>{one, "", "ABC", "abc"}),
          "SMEMBERS byte-exact unordered snapshot");
    check(call(d, "SADD", {key, one, "abc"}) == RespValue::integer(0), "SADD repeat noop");
    check(call(d, "SREM", {key, one, one, "never"}) == RespValue::integer(1),
          "SREM repeated member once");
    check(call(d, "SREM", {key, "", "ABC", "abc"}) == RespValue::integer(3),
          "SREM removes last members");
    check(call(d, "TYPE", {key}) == RespValue::simple_string("none"), "empty set removed");
}

void test_wrongtype_ttl_and_interop() {
    ManualClock clock;
    Database db(clock);
    auto d = dispatcher_for(db);
    const auto wrong =
        RespValue::error("WRONGTYPE Operation against a key holding the wrong kind of value");
    check(call(d, "SET", {"string", "1"}) == RespValue::simple_string("OK"), "string seed");
    check(call(d, "LPUSH", {"list", "v"}) == RespValue::integer(1), "list seed");
    check(call(d, "HSET", {"hash", "f", "v"}) == RespValue::integer(1), "hash seed");
    for (const auto& key : {"string", "list", "hash"}) {
        check(call(d, "SADD", {key, "x"}) == wrong, "SADD rejects wrong type");
        check(call(d, "SREM", {key, "x"}) == wrong, "SREM rejects wrong type");
        check(call(d, "SISMEMBER", {key, "x"}) == wrong, "SISMEMBER rejects wrong type");
        check(call(d, "SCARD", {key}) == wrong, "SCARD rejects wrong type");
        check(call(d, "SMEMBERS", {key}) == wrong, "SMEMBERS rejects wrong type");
        check(call(d, "SMISMEMBER", {key, "x"}) == wrong, "SMISMEMBER rejects wrong type");
    }
    check(call(d, "GET", {"string"}) == RespValue::bulk_string("1"), "string unchanged");
    check(call(d, "LLEN", {"list"}) == RespValue::integer(1), "list unchanged");
    check(call(d, "HLEN", {"hash"}) == RespValue::integer(1), "hash unchanged");
    check(call(d, "SADD", {"timed", "a", "b"}) == RespValue::integer(2), "set seed");
    check(call(d, "PEXPIRE", {"timed", "200"}) == RespValue::integer(1), "set TTL");
    const auto initial = db.expiration("timed");
    check(initial.has_value(), "set deadline installed");
    check(call(d, "SADD", {"timed", "b", "c"}) == RespValue::integer(1), "set add TTL");
    check(call(d, "SREM", {"timed", "a"}) == RespValue::integer(1), "set rem TTL");
    check(db.expiration("timed") == initial, "set mutation must preserve TTL");
    check(call(d, "GET", {"timed"}) == wrong, "GET set wrongtype");
    check(call(d, "LRANGE", {"timed", "0", "-1"}) == wrong, "LRANGE set wrongtype");
    check(call(d, "HGETALL", {"timed"}) == wrong, "HGETALL set wrongtype");
    check(call(d, "MGET", {"timed"}) == RespValue::array({RespValue::null_bulk_string()}),
          "MGET set nil");
    check(call(d, "SREM", {"timed", "b", "c"}) == RespValue::integer(2), "remove last TTL members");
    check(!db.expiration("timed") && db.type_of("timed") == StoredType::None,
          "last SREM cleans expiration index");
    clock.advance(201ms);
    check(db.sweep_expired(32).inspected == 0, "no dangling expiry index");
    check(call(d, "SADD", {"replace", "old"}) == RespValue::integer(1), "replace seed");
    check(call(d, "SET", {"replace", "new"}) == RespValue::simple_string("OK"), "SET replaces set");
    check(call(d, "SISMEMBER", {"replace", "old"}) == wrong, "type changed by SET");
    check(call(d, "SADD", {"overwrite", "old"}) == RespValue::integer(1), "MSET seed");
    check(call(d, "MSET", {"overwrite", "new"}) == RespValue::simple_string("OK"),
          "MSET replaces set");
    check(call(d, "GET", {"overwrite"}) == RespValue::bulk_string("new"), "MSET result");
    check(call(d, "SET", {"expired", "old", "PX", "10"}) == RespValue::simple_string("OK"),
          "expiring setup");
    clock.advance(11ms);
    check(call(d, "SADD", {"expired", "fresh"}) == RespValue::integer(1),
          "expired string recreated set");
    check(call(d, "PTTL", {"expired"}) == RespValue::integer(-1), "expired key must not leak TTL");
}

void test_arity_and_reply_limits() {
    Database db;
    auto d = dispatcher_for(db);
    const auto arity = [](const char* name) {
        return RespValue::error(std::string("ERR wrong number of arguments for '") + name +
                                "' command");
    };
    check(call(d, "SADD", {"k"}) == arity("sadd"), "SADD arity");
    check(call(d, "SREM", {"k"}) == arity("srem"), "SREM arity");
    check(call(d, "SISMEMBER", {"k"}) == arity("sismember"), "SISMEMBER arity");
    check(call(d, "SCARD", {"k", "m"}) == arity("scard"), "SCARD arity");
    check(call(d, "SMEMBERS", {"k", "m"}) == arity("smembers"), "SMEMBERS arity");
    check(call(d, "SMISMEMBER", {"k"}) == arity("smismember"), "SMISMEMBER arity");
    check(call(d, "SADD", {"too-large", std::string(8177, 'x')}) ==
              RespValue::error("ERR set member exceeds the 8176-byte reply-safe limit"),
          "reject oversize member");
    check(!db.exists("too-large"), "oversize SADD must not modify DB");
    check(call(d, "SADD", {"wide", std::string(4090, 'a')}) == RespValue::integer(1), "wide a");
    check(call(d, "SADD", {"wide", std::string(4090, 'b')}) == RespValue::integer(1), "wide b");
    check(call(d, "SMEMBERS", {"wide"}) ==
              RespValue::error("ERR SMEMBERS response exceeds the 8192-byte limit"),
          "bounded SMEMBERS");
    check(call(d, "SISMEMBER", {"wide", std::string(4090, 'a')}) == RespValue::integer(1),
          "budget failure did not mutate set");
    std::vector<std::string> huge(2100, "m");
    huge.insert(huge.begin(), "wide");
    check(call(d, "SMISMEMBER", std::move(huge)) ==
              RespValue::error("ERR SMISMEMBER response exceeds the 8192-byte limit"),
          "bounded SMISMEMBER");
    check(call(d, "SCARD", {"wide"}) == RespValue::integer(2), "bounds do not mutate set");
}
} // namespace

int main() {
    try {
        test_binary_membership_and_unordered();
        test_wrongtype_ttl_and_interop();
        test_arity_and_reply_limits();
        std::cout << "PASS: Phase 6D.2 six RESP2 set commands, TTL, wrongtype, bounds\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
