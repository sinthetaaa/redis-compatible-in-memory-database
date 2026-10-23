#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/commands/HashCommands.hpp"
#include "redisdb/commands/KeyCommands.hpp"
#include "redisdb/commands/ListCommands.hpp"
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

void check(bool ok, const char* note) {
    if (!ok) {
        throw std::runtime_error(note);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return now_; }
    void advance(std::chrono::milliseconds by) { now_ += by; }

  private:
    WallTimePoint now_{WallTimePoint{} + 1000s};
};

CommandDispatcher make_dispatcher(Database& db) {
    auto routes = redisdb::commands::string_routes(db);
    for (auto& route : redisdb::commands::key_routes(db)) {
        routes.push_back(std::move(route));
    }
    for (auto& route : redisdb::commands::list_routes(db)) {
        routes.push_back(std::move(route));
    }
    for (auto& route : redisdb::commands::hash_routes(db)) {
        routes.push_back(std::move(route));
    }
    return CommandDispatcher(std::move(routes));
}

RespValue call(const CommandDispatcher& dispatcher, std::string verb,
               std::vector<std::string> args = {}) {
    std::vector<RespValue> request;
    request.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& arg : args) {
        request.push_back(RespValue::bulk_string(std::move(arg)));
    }
    const auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(request)));
    check(decoded.command.has_value(), "hash command decode");
    const auto result = dispatcher.dispatch(*decoded.command);
    check(result.handled.has_value(), "hash command missing from route table");
    return result.handled->reply;
}

std::unordered_map<std::string, std::string> map_of(const RespValue& response) {
    const auto* array = std::get_if<redisdb::protocol::Array>(&response.storage());
    check(array && array->elements.size() % 2 == 0, "HGETALL must be paired RESP2 array");
    std::unordered_map<std::string, std::string> result;
    for (std::size_t i = 0; i < array->elements.size(); i += 2) {
        const auto* field =
            std::get_if<redisdb::protocol::BulkString>(&array->elements[i].storage());
        const auto* value =
            std::get_if<redisdb::protocol::BulkString>(&array->elements[i + 1].storage());
        check(field && value, "HGETALL requires bulk-string field/value pairs");
        result.insert_or_assign(field->bytes, value->bytes);
    }
    return result;
}

void test_seven_commands_binary_and_unordered() {
    Database db;
    const auto dispatcher = make_dispatcher(db);
    const std::string key("k\0\xff", 3);
    const std::string field("f\0\x80", 3);
    const std::string bytes("v\0\xfe\r\n", 5);
    check(call(dispatcher, "HGET", {key, field}) == RespValue::null_bulk_string(), "missing HGET");
    check(call(dispatcher, "HEXISTS", {key, field}) == RespValue::integer(0), "missing HEXISTS");
    check(call(dispatcher, "HLEN", {key}) == RespValue::integer(0), "missing HLEN");
    check(call(dispatcher, "HDEL", {key, field}) == RespValue::integer(0), "missing HDEL");
    check(call(dispatcher, "HMGET", {key, field, "other"}) ==
              RespValue::array({RespValue::null_bulk_string(), RespValue::null_bulk_string()}),
          "missing HMGET");
    check(map_of(call(dispatcher, "HGETALL", {key})).empty(), "missing HGETALL");
    check(call(dispatcher, "hSeT", {key, field, "old", "other", "", field, bytes}) ==
              RespValue::integer(2),
          "HSET duplicate new count");
    check(call(dispatcher, "TYPE", {key}) == RespValue::simple_string("hash"), "TYPE hash");
    check(call(dispatcher, "HGET", {key, field}) == RespValue::bulk_string(bytes), "binary HGET");
    check(call(dispatcher, "HMGET", {key, "other", field, "missing", field}) ==
              RespValue::array({RespValue::bulk_string(""), RespValue::bulk_string(bytes),
                                RespValue::null_bulk_string(), RespValue::bulk_string(bytes)}),
          "HMGET order, repeats and nil");
    check(call(dispatcher, "HSET", {key, field, "new", "third", "3", "third", "4"}) ==
              RespValue::integer(1),
          "HSET updated/new field count");
    check(call(dispatcher, "HLEN", {key}) == RespValue::integer(3), "HLEN count");
    check(call(dispatcher, "HEXISTS", {key, field}) == RespValue::integer(1), "HEXISTS true");
    check(map_of(call(dispatcher, "HGETALL", {key})) ==
              (std::unordered_map<std::string, std::string>{
                  {field, "new"}, {"other", ""}, {"third", "4"}}),
          "HGETALL pair semantics regardless of hash iteration order");
    check(call(dispatcher, "HDEL", {key, "absent", field, field}) == RespValue::integer(1),
          "HDEL duplicate and missing");
    check(call(dispatcher, "HDEL", {key, "other", "third"}) == RespValue::integer(2),
          "HDEL remaining fields");
    check(call(dispatcher, "TYPE", {key}) == RespValue::simple_string("none"),
          "last HDEL deletes empty hash");
}

void test_wrongtype_ttl_and_string_list_interop() {
    ManualClock clock;
    Database db(clock);
    const auto dispatcher = make_dispatcher(db);
    const RespValue wrong =
        RespValue::error("WRONGTYPE Operation against a key holding the wrong kind of value");
    check(call(dispatcher, "SET", {"str", "42"}) == RespValue::simple_string("OK"), "string setup");
    check(call(dispatcher, "LPUSH", {"list", "a"}) == RespValue::integer(1), "list setup");
    for (const auto& key : {"str", "list"}) {
        check(call(dispatcher, "HSET", {key, "f", "v"}) == wrong, "HSET type");
        check(call(dispatcher, "HGET", {key, "f"}) == wrong, "HGET type");
        check(call(dispatcher, "HEXISTS", {key, "f"}) == wrong, "HEXISTS type");
        check(call(dispatcher, "HLEN", {key}) == wrong, "HLEN type");
        check(call(dispatcher, "HDEL", {key, "f"}) == wrong, "HDEL type");
        check(call(dispatcher, "HMGET", {key, "f"}) == wrong, "HMGET type");
        check(call(dispatcher, "HGETALL", {key}) == wrong, "HGETALL type");
    }
    check(call(dispatcher, "GET", {"str"}) == RespValue::bulk_string("42"), "string survives");
    check(call(dispatcher, "LRANGE", {"list", "0", "-1"}) ==
              RespValue::array({RespValue::bulk_string("a")}),
          "list survives");
    check(call(dispatcher, "HSET", {"hash", "f", "v"}) == RespValue::integer(1), "hash seed");
    check(call(dispatcher, "GET", {"hash"}) == wrong, "GET hash wrongtype");
    check(call(dispatcher, "LLEN", {"hash"}) == wrong, "LLEN hash wrongtype");
    check(call(dispatcher, "MGET", {"hash", "str"}) ==
              RespValue::array({RespValue::null_bulk_string(), RespValue::bulk_string("42")}),
          "MGET hash nil");
    check(call(dispatcher, "PEXPIRE", {"hash", "500"}) == RespValue::integer(1),
          "hash expiration setup");
    const auto deadline = db.expiration("hash");
    check(deadline.has_value(), "hash deadline exists");
    check(call(dispatcher, "HSET", {"hash", "g", "x"}) == RespValue::integer(1),
          "HSET preserves TTL");
    check(db.expiration("hash") == deadline, "HSET changed TTL");
    check(call(dispatcher, "HDEL", {"hash", "f"}) == RespValue::integer(1), "HDEL partial");
    check(db.expiration("hash") == deadline, "HDEL partial changed TTL");
    check(call(dispatcher, "HDEL", {"hash", "g"}) == RespValue::integer(1), "HDEL final");
    check(!db.expiration("hash") && db.type_of("hash") == redisdb::storage::StoredType::None,
          "final HDEL must clear TTL index");
    clock.advance(500ms);
    check(db.sweep_expired(32).inspected == 0, "HDEL left stale expiration index");
    check(call(dispatcher, "HSET", {"convert", "f", "old"}) == RespValue::integer(1),
          "replaceable hash");
    check(call(dispatcher, "SET", {"convert", "replaced"}) == RespValue::simple_string("OK"),
          "SET replaces hash");
    check(call(dispatcher, "TYPE", {"convert"}) == RespValue::simple_string("string"),
          "TYPE after SET");
    check(call(dispatcher, "HGET", {"convert", "f"}) == wrong, "HGET after SET");
    check(call(dispatcher, "SET", {"expires", "x", "PX", "10"}) == RespValue::simple_string("OK"),
          "expired-string setup");
    clock.advance(11ms);
    check(call(dispatcher, "HSET", {"expires", "f", "fresh"}) == RespValue::integer(1),
          "expired string becomes new hash");
    check(call(dispatcher, "PTTL", {"expires"}) == RespValue::integer(-1),
          "recreated hash should not inherit expired TTL");
}

void test_arity_and_reply_budget() {
    Database db;
    const auto dispatcher = make_dispatcher(db);
    check(call(dispatcher, "HSET", {"k", "f"}) ==
              RespValue::error("ERR wrong number of arguments for 'hset' command"),
          "HSET arity");
    check(call(dispatcher, "HSET", {"k", "f", "v", "incomplete"}) ==
              RespValue::error("ERR wrong number of arguments for 'hset' command"),
          "HSET odd pair");
    check(call(dispatcher, "HGET", {"k"}) ==
              RespValue::error("ERR wrong number of arguments for 'hget' command"),
          "HGET arity");
    check(call(dispatcher, "HEXISTS", {"k"}) ==
              RespValue::error("ERR wrong number of arguments for 'hexists' command"),
          "HEXISTS arity");
    check(call(dispatcher, "HLEN", {"k", "f"}) ==
              RespValue::error("ERR wrong number of arguments for 'hlen' command"),
          "HLEN arity");
    check(call(dispatcher, "HDEL", {"k"}) ==
              RespValue::error("ERR wrong number of arguments for 'hdel' command"),
          "HDEL arity");
    check(call(dispatcher, "HMGET", {"k"}) ==
              RespValue::error("ERR wrong number of arguments for 'hmget' command"),
          "HMGET arity");
    check(call(dispatcher, "HGETALL", {"k", "f"}) ==
              RespValue::error("ERR wrong number of arguments for 'hgetall' command"),
          "HGETALL arity");
    check(call(dispatcher, "HSET", {"over", "field", std::string(8177, 'x')}) ==
              RespValue::error("ERR hash field or value exceeds the 8176-byte reply-safe limit"),
          "HSET oversized field value");
    check(!db.exists("over"), "invalid HSET mutated key");
    check(call(dispatcher, "HSET", {"too-long", std::string(8177, 'f'), "x"}) ==
              RespValue::error("ERR hash field or value exceeds the 8176-byte reply-safe limit"),
          "HSET oversized field");
    check(!db.exists("too-long"), "invalid field mutated key");
    check(call(dispatcher, "HSET",
               {"wide", "a", std::string(4090, 'a'), "b", std::string(4090, 'b')}) ==
              RespValue::integer(2),
          "large HSET seed");
    check(call(dispatcher, "HGETALL", {"wide"}) ==
              RespValue::error("ERR HGETALL response exceeds the 8192-byte limit"),
          "HGETALL must cap response");
    check(call(dispatcher, "HMGET", {"wide", "a", "b"}) ==
              RespValue::error("ERR HMGET response exceeds the 8192-byte limit"),
          "HMGET must cap response");
    check(call(dispatcher, "HGET", {"wide", "a"}) == RespValue::bulk_string(std::string(4090, 'a')),
          "HGET succeeds independently");
    check(call(dispatcher, "HLEN", {"wide"}) == RespValue::integer(2),
          "failed reply must not modify hash");
}
} // namespace

int main() {
    try {
        test_seven_commands_binary_and_unordered();
        test_wrongtype_ttl_and_string_list_interop();
        test_arity_and_reply_budget();
        std::cout << "PASS: Phase 6C.2 seven RESP2 hash routes, TTL, WRONGTYPE and bounds\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
