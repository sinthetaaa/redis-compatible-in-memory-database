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
#include <utility>
#include <vector>

namespace {
using redisdb::commands::CommandDispatcher;
using redisdb::protocol::RespValue;
using redisdb::storage::Clock;
using redisdb::storage::Database;
using redisdb::storage::MonotonicTimePoint;
using redisdb::storage::WallTimePoint;
using namespace std::chrono_literals;

void check(bool condition, const char* description) {
    if (!condition) {
        throw std::runtime_error(description);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return now_; }
    void advance(std::chrono::milliseconds offset) { now_ += offset; }

  private:
    WallTimePoint now_{WallTimePoint{} + 1000s};
};

CommandDispatcher make_dispatcher(Database& database) {
    auto routes = redisdb::commands::string_routes(database);
    for (auto& route : redisdb::commands::key_routes(database)) {
        routes.push_back(std::move(route));
    }
    for (auto& route : redisdb::commands::list_routes(database)) {
        routes.push_back(std::move(route));
    }
    for (auto& route : redisdb::commands::hash_routes(database)) {
        routes.push_back(std::move(route));
    }
    for (auto& route : redisdb::commands::set_routes(database)) {
        routes.push_back(std::move(route));
    }
    return CommandDispatcher(std::move(routes));
}

RespValue call(const CommandDispatcher& dispatcher, std::string verb,
               std::vector<std::string> arguments = {}) {
    std::vector<RespValue> encoded;
    encoded.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& item : arguments) {
        encoded.push_back(RespValue::bulk_string(std::move(item)));
    }
    const auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(encoded)));
    check(decoded.command.has_value(), "decode valid command");
    auto result = dispatcher.dispatch(*decoded.command);
    check(result.handled.has_value(), "route must be registered");
    return std::move(result.handled->reply);
}

struct Operation {
    const char* owner;
    const char* name;
    std::vector<std::string> tail;
};

std::vector<Operation> operations() {
    return {
        {"string", "GET", {}},
        {"string", "APPEND", {"x"}},
        {"string", "STRLEN", {}},
        {"string", "INCR", {}},
        {"string", "DECR", {}},
        {"string", "INCRBY", {"3"}},
        {"string", "DECRBY", {"3"}},
        {"list", "LPUSH", {"new"}},
        {"list", "RPUSH", {"new"}},
        {"list", "LLEN", {}},
        {"list", "LPOP", {}},
        {"list", "RPOP", {}},
        {"list", "LRANGE", {"0", "-1"}},
        {"hash", "HSET", {"field", "new"}},
        {"hash", "HGET", {"field"}},
        {"hash", "HEXISTS", {"field"}},
        {"hash", "HLEN", {}},
        {"hash", "HDEL", {"field"}},
        {"hash", "HMGET", {"field"}},
        {"hash", "HGETALL", {}},
        {"set", "SADD", {"new"}},
        {"set", "SREM", {"member"}},
        {"set", "SISMEMBER", {"member"}},
        {"set", "SCARD", {}},
        {"set", "SMEMBERS", {}},
        {"set", "SMISMEMBER", {"member", "no"}},
    };
}

struct TypedKey {
    std::string kind;
    std::string key;
    std::string read;
    std::vector<std::string> read_tail;
};

std::vector<TypedKey> seed(const CommandDispatcher& dispatcher) {
    const std::vector<TypedKey> typed = {
        {"string", std::string("str\0\xff", 5), "GET", {}},
        {"list", std::string("list\0\xff", 6), "LRANGE", {"0", "-1"}},
        {"hash", std::string("hash\0\xff", 6), "HGETALL", {}},
        {"set", std::string("set\0\xff", 5), "SMEMBERS", {}},
    };
    check(call(dispatcher, "SET", {typed[0].key, "19"}) == RespValue::simple_string("OK"),
          "seed string");
    check(call(dispatcher, "RPUSH", {typed[1].key, "first", "second"}) == RespValue::integer(2),
          "seed list");
    check(call(dispatcher, "HSET", {typed[2].key, "field", "value"}) == RespValue::integer(1),
          "seed hash");
    check(call(dispatcher, "SADD", {typed[3].key, "member", "other"}) == RespValue::integer(2),
          "seed set");
    for (const auto& item : typed) {
        check(call(dispatcher, "PEXPIRE", {item.key, "120000"}) == RespValue::integer(1),
              "seed expiry");
        check(call(dispatcher, "TYPE", {item.key}) == RespValue::simple_string(item.kind),
              "type seed");
        check(call(dispatcher, "PTTL", {item.key}) == RespValue::integer(120000), "initial TTL");
    }
    return typed;
}

RespValue read_value(const CommandDispatcher& dispatcher, const TypedKey& item) {
    std::vector<std::string> arguments{item.key};
    arguments.insert(arguments.end(), item.read_tail.begin(), item.read_tail.end());
    return call(dispatcher, item.read, std::move(arguments));
}

void test_exhaustive_wrongtype_and_rollback() {
    ManualClock clock;
    Database database(clock);
    const auto dispatcher = make_dispatcher(database);
    const auto typed = seed(dispatcher);
    const auto wrong =
        RespValue::error("WRONGTYPE Operation against a key holding the wrong kind of value");
    std::size_t cases = 0;
    for (const auto& item : typed) {
        const auto original = read_value(dispatcher, item);
        for (const auto& operation : operations()) {
            if (item.kind == operation.owner) {
                continue;
            }
            std::vector<std::string> arguments{item.key};
            arguments.insert(arguments.end(), operation.tail.begin(), operation.tail.end());
            check(call(dispatcher, operation.name, std::move(arguments)) == wrong,
                  "foreign typed command must return exact WRONGTYPE");
            check(call(dispatcher, "TYPE", {item.key}) == RespValue::simple_string(item.kind),
                  "WRONGTYPE must not replace stored type");
            check(read_value(dispatcher, item) == original,
                  "WRONGTYPE must not mutate original bytes or collection");
            check(call(dispatcher, "PTTL", {item.key}) == RespValue::integer(120000),
                  "WRONGTYPE must not remove or change TTL");
            check(call(dispatcher, "EXISTS", {item.key}) == RespValue::integer(1),
                  "WRONGTYPE must not delete key");
            ++cases;
        }
    }
    check(cases == 78, "expected complete 26-by-three type mismatch matrix");
    check(call(dispatcher, "MGET",
               {typed[0].key, typed[1].key, typed[2].key, typed[3].key, "not-present",
                typed[0].key}) ==
              RespValue::array({RespValue::bulk_string("19"), RespValue::null_bulk_string(),
                                RespValue::null_bulk_string(), RespValue::null_bulk_string(),
                                RespValue::null_bulk_string(), RespValue::bulk_string("19")}),
          "MGET returns nil for nonstrings while preserving positions and duplicates");
    check(call(dispatcher, "TYPE", {"not-present"}) == RespValue::simple_string("none"),
          "missing TYPE none");
    check(call(dispatcher, "PTTL", {"not-present"}) == RespValue::integer(-2), "missing PTTL -2");
}

void test_missing_expiry_and_allowed_replacement() {
    ManualClock clock;
    Database database(clock);
    const auto dispatcher = make_dispatcher(database);
    const auto missing = std::string("missing\0\xfe", 9);
    check(call(dispatcher, "GET", {missing}) == RespValue::null_bulk_string(), "GET missing");
    check(call(dispatcher, "LLEN", {missing}) == RespValue::integer(0), "LLEN missing");
    check(call(dispatcher, "HLEN", {missing}) == RespValue::integer(0), "HLEN missing");
    check(call(dispatcher, "SCARD", {missing}) == RespValue::integer(0), "SCARD missing");
    check(call(dispatcher, "LRANGE", {missing, "0", "-1"}) == RespValue::array({}),
          "LRANGE missing empty array");
    check(call(dispatcher, "HGET", {missing, "field"}) == RespValue::null_bulk_string(),
          "HGET missing nil");
    check(call(dispatcher, "SISMEMBER", {missing, "member"}) == RespValue::integer(0),
          "SISMEMBER missing zero");
    check(call(dispatcher, "SMISMEMBER", {missing, "x", "y"}) ==
              RespValue::array({RespValue::integer(0), RespValue::integer(0)}),
          "SMISMEMBER missing positional zeros");
    auto typed = seed(dispatcher);
    for (const auto& item : typed) {
        if (item.kind == "string") {
            continue;
        }
        check(call(dispatcher, "SET", {item.key, "new"}) == RespValue::simple_string("OK"),
              "SET replacement is intentionally supported");
        check(call(dispatcher, "GET", {item.key}) == RespValue::bulk_string("new"),
              "SET replacement value");
        check(call(dispatcher, "PTTL", {item.key}) == RespValue::integer(-1),
              "SET replacement clears TTL");
    }
}

void test_mset_and_lazy_expiry() {
    ManualClock clock;
    Database database(clock);
    const auto dispatcher = make_dispatcher(database);
    auto typed = seed(dispatcher);
    check(call(dispatcher, "MSET",
               {typed[1].key, "new-list", typed[2].key, "new-hash", typed[3].key, "new-set"}) ==
              RespValue::simple_string("OK"),
          "MSET intentionally replaces collection types");
    for (std::size_t i = 1; i < typed.size(); ++i) {
        check(call(dispatcher, "TYPE", {typed[i].key}) == RespValue::simple_string("string"),
              "MSET replacement type");
        check(call(dispatcher, "PTTL", {typed[i].key}) == RespValue::integer(-1),
              "MSET replacement clears TTL");
    }
    clock.advance(120001ms);
    check(call(dispatcher, "TYPE", {typed[0].key}) == RespValue::simple_string("none"),
          "elapsed TTL makes untouched key missing");
    check(call(dispatcher, "GET", {typed[0].key}) == RespValue::null_bulk_string(),
          "expired string must behave missing");
    check(call(dispatcher, "PTTL", {typed[0].key}) == RespValue::integer(-2),
          "expired key has PTTL -2");
}
} // namespace

int main() {
    try {
        test_exhaustive_wrongtype_and_rollback();
        test_missing_expiry_and_allowed_replacement();
        test_mset_and_lazy_expiry();
        std::cout << "PASS: Phase 6E 78-case WRONGTYPE type-family matrix and rollback\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
