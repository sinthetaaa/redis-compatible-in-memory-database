#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
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
void check(bool condition, const char* description) {
    if (!condition) {
        throw std::runtime_error(description);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return now_; }
    void advance(std::chrono::milliseconds ms) { now_ += ms; }

  private:
    WallTimePoint now_{WallTimePoint{} + 1000s};
};

CommandDispatcher make_dispatcher(Database& db) {
    auto routes = redisdb::commands::string_routes(db);
    for (auto& r : redisdb::commands::key_routes(db)) {
        routes.push_back(std::move(r));
    }
    for (auto& r : redisdb::commands::list_routes(db)) {
        routes.push_back(std::move(r));
    }
    return CommandDispatcher(std::move(routes));
}

RespValue call(const CommandDispatcher& dispatcher, std::string name,
               std::vector<std::string> args = {}) {
    std::vector<RespValue> values;
    values.push_back(RespValue::bulk_string(std::move(name)));
    for (auto& arg : args) {
        values.push_back(RespValue::bulk_string(std::move(arg)));
    }
    auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(values)));
    check(decoded.command.has_value(), "decode request failed");
    auto response = dispatcher.dispatch(*decoded.command);
    check(response.handled.has_value(), "list route not registered");
    return std::move(response.handled->reply);
}

RespValue array_of(const std::vector<std::string>& elements) {
    std::vector<RespValue> reply;
    for (const auto& bytes : elements) {
        reply.push_back(RespValue::bulk_string(bytes));
    }
    return RespValue::array(std::move(reply));
}

void test_order_binary_negative_indexes() {
    Database db;
    const auto dispatcher = make_dispatcher(db);
    const std::string key("k\0\xff", 3);
    const std::string binary("A\0\x80\xff", 4);
    check(call(dispatcher, "LLEN", {key}) == RespValue::integer(0), "missing LLEN");
    check(call(dispatcher, "LPOP", {key}) == RespValue::null_bulk_string(), "missing LPOP");
    check(call(dispatcher, "LRANGE", {key, "0", "-1"}) == array_of({}), "missing LRANGE");
    check(call(dispatcher, "rPuSh", {key, "one", binary, "one"}) == RespValue::integer(3), "RPUSH");
    check(call(dispatcher, "LPUSH", {key, "left1", "left2"}) == RespValue::integer(5),
          "LPUSH order");
    check(call(dispatcher, "TYPE", {key}) == RespValue::simple_string("list"), "TYPE list");
    check(call(dispatcher, "LLEN", {key}) == RespValue::integer(5), "LLEN 5");
    check(call(dispatcher, "LRANGE", {key, "0", "-1"}) ==
              array_of({"left2", "left1", "one", binary, "one"}),
          "full LRANGE");
    check(call(dispatcher, "LRANGE", {key, "1", "-2"}) == array_of({"left1", "one", binary}),
          "interior LRANGE");
    check(call(dispatcher, "LRANGE", {key, "-2", "-1"}) == array_of({binary, "one"}),
          "negative LRANGE");
    check(call(dispatcher, "LRANGE", {key, "1", "1"}) == array_of({"left1"}), "singleton");
    check(call(dispatcher, "LRANGE", {key, "9", "20"}) == array_of({}), "start too far");
    check(call(dispatcher, "LRANGE", {key, "3", "1"}) == array_of({}), "reverse bounds");
    check(call(dispatcher, "LRANGE", {key, "-9223372036854775808", "9223372036854775807"}) ==
              array_of({"left2", "left1", "one", binary, "one"}),
          "extreme indices");
    check(call(dispatcher, "LRANGE", {key, "0", "-9223372036854775808"}) == array_of({}),
          "extreme negative stop");
    check(call(dispatcher, "LPOP", {key}) == RespValue::bulk_string("left2"), "LPOP left");
    check(call(dispatcher, "RPOP", {key}) == RespValue::bulk_string("one"), "RPOP right");
    check(call(dispatcher, "LPOP", {key}) == RespValue::bulk_string("left1"), "LPOP 2");
    check(call(dispatcher, "LPOP", {key}) == RespValue::bulk_string("one"), "LPOP duplicate");
    check(call(dispatcher, "RPOP", {key}) == RespValue::bulk_string(binary), "RPOP binary");
    check(call(dispatcher, "TYPE", {key}) == RespValue::simple_string("none"), "last POP deletes");
}

void test_wrong_types_replace_ttl() {
    ManualClock clock;
    Database db(clock);
    const auto dispatcher = make_dispatcher(db);
    const auto wrong =
        RespValue::error("WRONGTYPE Operation against a key holding the wrong kind of value");
    check(call(dispatcher, "SET", {"str", "10"}) == RespValue::simple_string("OK"), "set str");
    for (const auto& name : {"LPUSH", "RPUSH"}) {
        check(call(dispatcher, name, {"str", "a"}) == wrong, "push string wrongtype");
    }
    for (const auto& name : {"LLEN", "LPOP", "RPOP"}) {
        check(call(dispatcher, name, {"str"}) == wrong, "read string wrongtype");
    }
    check(call(dispatcher, "LRANGE", {"str", "0", "-1"}) == wrong, "range string wrongtype");
    check(call(dispatcher, "GET", {"str"}) == RespValue::bulk_string("10"),
          "wrongtype mutated str");
    check(call(dispatcher, "LPUSH", {"list", "x"}) == RespValue::integer(1), "seed list");
    check(call(dispatcher, "GET", {"list"}) == wrong, "GET list wrongtype");
    check(call(dispatcher, "MGET", {"list", "str"}) ==
              RespValue::array({RespValue::null_bulk_string(), RespValue::bulk_string("10")}),
          "MGET non-string nil");
    check(call(dispatcher, "APPEND", {"list", "x"}) == wrong, "APPEND list wrongtype");
    check(call(dispatcher, "STRLEN", {"list"}) == wrong, "STRLEN list wrongtype");
    check(call(dispatcher, "INCR", {"list"}) == wrong, "INCR list wrongtype");
    check(call(dispatcher, "PEXPIRE", {"list", "5000"}) == RespValue::integer(1), "list TTL");
    const auto deadline = db.expiration("list");
    check(deadline.has_value(), "list lost TTL");
    check(call(dispatcher, "RPUSH", {"list", "y"}) == RespValue::integer(2), "list append TTL");
    check(db.expiration("list") == deadline, "RPUSH lost deadline");
    check(call(dispatcher, "LPOP", {"list"}) == RespValue::bulk_string("x"), "list pop TTL");
    check(db.expiration("list") == deadline, "POP lost deadline");
    check(call(dispatcher, "RPOP", {"list"}) == RespValue::bulk_string("y"), "remove final");
    check(!db.expiration("list") && !db.exists("list"), "last pop left expiry record");
    check(call(dispatcher, "LPUSH", {"list", "new"}) == RespValue::integer(1), "recreate");
    check(call(dispatcher, "SET", {"list", "overwrite"}) == RespValue::simple_string("OK"),
          "replace list");
    check(call(dispatcher, "TYPE", {"list"}) == RespValue::simple_string("string"),
          "TYPE string after SET");
    check(call(dispatcher, "RPUSH", {"list", "x"}) == wrong, "cannot RPUSH new string");
    check(call(dispatcher, "SET", {"exp", "x", "PX", "10"}) == RespValue::simple_string("OK"),
          "expire seed");
    clock.advance(11ms);
    check(call(dispatcher, "LPUSH", {"exp", "y"}) == RespValue::integer(1),
          "expired string becomes list");
    check(call(dispatcher, "PTTL", {"exp"}) == RespValue::integer(-1),
          "expired key TTL not cleared");
    check(call(dispatcher, "MSET", {"list", "mset"}) == RespValue::simple_string("OK"),
          "MSET replace str");
    check(call(dispatcher, "LPUSH", {"mset-list", "x"}) == RespValue::integer(1),
          "MSET list setup");
    check(call(dispatcher, "MSET", {"mset-list", "42"}) == RespValue::simple_string("OK"),
          "MSET replace list");
    check(call(dispatcher, "GET", {"mset-list"}) == RespValue::bulk_string("42"),
          "MSET typed overwrite");
}

void test_bad_requests_and_bounded_replies() {
    Database db;
    const auto dispatcher = make_dispatcher(db);
    for (const auto& name : {"LPUSH", "RPUSH"}) {
        check(call(dispatcher, name, {"k"}) ==
                  RespValue::error(std::string("ERR wrong number of arguments for '") +
                                   (name == std::string("LPUSH") ? "lpush" : "rpush") +
                                   "' command"),
              "push arity");
    }
    check(call(dispatcher, "LLEN") ==
              RespValue::error("ERR wrong number of arguments for 'llen' command"),
          "LLEN arity");
    check(call(dispatcher, "LPOP", {"k", "1"}) ==
              RespValue::error("ERR wrong number of arguments for 'lpop' command"),
          "LPOP arity");
    check(call(dispatcher, "RPOP", {"k", "1"}) ==
              RespValue::error("ERR wrong number of arguments for 'rpop' command"),
          "RPOP arity");
    check(call(dispatcher, "LRANGE", {"k", "0"}) ==
              RespValue::error("ERR wrong number of arguments for 'lrange' command"),
          "LRANGE arity");
    check(call(dispatcher, "LRANGE", {"k", "bad", "-1"}) ==
              RespValue::error("ERR value is not an integer or out of range"),
          "bad LRANGE integer");
    check(call(dispatcher, "LRANGE", {"k", "0", "9223372036854775808"}) ==
              RespValue::error("ERR value is not an integer or out of range"),
          "overflow LRANGE integer");
    check(call(dispatcher, "LPUSH", {"too-large", std::string(8177, 'x')}) ==
              RespValue::error("ERR list element exceeds the 8176-byte reply-safe limit"),
          "oversize element");
    check(!db.exists("too-large"), "oversize push partially mutated");
    check(call(dispatcher, "RPUSH", {"many", std::string(4090, 'a'), std::string(4090, 'b')}) ==
              RespValue::integer(2),
          "large elements seed");
    check(call(dispatcher, "LRANGE", {"many", "0", "-1"}) ==
              RespValue::error("ERR LRANGE response exceeds the 8192-byte limit"),
          "bounded LRANGE");
    check(call(dispatcher, "LLEN", {"many"}) == RespValue::integer(2),
          "oversize range mutated list");
    check(call(dispatcher, "LPOP", {"many"}) == RespValue::bulk_string(std::string(4090, 'a')),
          "LPOP after failed range");
}
} // namespace

int main() {
    try {
        test_order_binary_negative_indexes();
        test_wrong_types_replace_ttl();
        test_bad_requests_and_bounded_replies();
        std::cout << "PASS: Phase 6B.2 six list commands, type safety, TTL and reply limits\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
