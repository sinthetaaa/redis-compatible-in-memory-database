#include "redisdb/commands/StringCommands.hpp"

#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/protocol/RespValue.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using redisdb::commands::CommandDispatcher;
using redisdb::protocol::RespValue;
using redisdb::storage::Database;

namespace {
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

RespValue call(const CommandDispatcher& dispatcher, std::string verb,
               std::vector<std::string> args = {}) {
    std::vector<RespValue> request;
    request.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& arg : args) {
        request.push_back(RespValue::bulk_string(std::move(arg)));
    }
    auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(request)));
    check(decoded.command.has_value(), "decode failed");
    auto outcome = dispatcher.dispatch(*decoded.command);
    check(outcome.handled.has_value(), "integer command was not registered");
    check(outcome.handled->disposition == redisdb::commands::ConnectionDisposition::KeepOpen,
          "integer command unexpectedly closed connection");
    return std::move(outcome.handled->reply);
}

void test_integer_commands() {
    Database db;
    const CommandDispatcher dispatcher(redisdb::commands::string_routes(db));
    const auto number = [](std::int64_t n) { return RespValue::integer(n); };
    const auto invalid = RespValue::error("ERR value is not an integer or out of range");
    const auto overflow = RespValue::error("ERR increment or decrement would overflow");

    check(call(dispatcher, "INCR", {"counter"}) == number(1), "INCR missing");
    check(call(dispatcher, "iNcR", {"counter"}) == number(2), "INCR again");
    check(call(dispatcher, "DECR", {"counter"}) == number(1), "DECR existing");
    check(call(dispatcher, "INCRBY", {"counter", "12"}) == number(13), "INCRBY positive");
    check(call(dispatcher, "DECRBY", {"counter", "5"}) == number(8), "DECRBY positive");
    check(call(dispatcher, "INCRBY", {"counter", "-10"}) == number(-2), "INCRBY negative");
    check(call(dispatcher, "DECRBY", {"counter", "-5"}) == number(3), "DECRBY negative");
    check(call(dispatcher, "GET", {"counter"}) == RespValue::bulk_string("3"),
          "integer must be stored as decimal text");
    check(call(dispatcher, "DECR", {"negative-missing"}) == number(-1),
          "DECR missing starts at zero");
    check(call(dispatcher, "DECRBY", {"new-zero", "0"}) == number(0),
          "DECRBY missing zero creates the key");
    check(db.exists("new-zero") && db.get("new-zero") == "0", "missing key creation");

    const std::string binary_key("c\0\xff\r\n", 5);
    check(call(dispatcher, "INCR", {binary_key}) == number(1), "binary key INCR");
    check(call(dispatcher, "DECRBY", {binary_key, "3"}) == number(-2), "binary key DECRBY");
    check(db.get(binary_key) == "-2", "binary key value stored incorrectly");
    check(call(dispatcher, "INCR", {""}) == number(1), "empty key INCR");
    check(call(dispatcher, "INCR", {"COUNTER"}) == number(1), "case-sensitive key");

    const std::vector<std::string> bad_values = {"",
                                                 "+1",
                                                 "01",
                                                 "-0",
                                                 "-01",
                                                 " 1",
                                                 "1 ",
                                                 "1.0",
                                                 "1e2",
                                                 "x",
                                                 "1\n",
                                                 "18446744073709551616",
                                                 std::string("1\0", 2)};
    for (std::size_t i = 0; i < bad_values.size(); ++i) {
        const std::string key = "bad-" + std::to_string(i);
        check(call(dispatcher, "SET", {key, bad_values[i]}) == RespValue::simple_string("OK"),
              "bad-value setup");
        check(call(dispatcher, "INCR", {key}) == invalid, "bad stored number accepted");
        check(db.get(key) == bad_values[i], "failed INCR corrupted original bytes");
        check(call(dispatcher, "INCRBY", {"uncreated", bad_values[i]}) == invalid,
              "bad operand accepted");
        check(!db.exists("uncreated"), "bad operand created absent key");
    }

    check(call(dispatcher, "INCR") ==
              RespValue::error("ERR wrong number of arguments for 'incr' command"),
          "INCR missing arity");
    check(call(dispatcher, "DECR", {"counter", "extra"}) ==
              RespValue::error("ERR wrong number of arguments for 'decr' command"),
          "DECR excess arity");
    check(call(dispatcher, "INCRBY", {"counter"}) ==
              RespValue::error("ERR wrong number of arguments for 'incrby' command"),
          "INCRBY missing operand");
    check(call(dispatcher, "DECRBY", {"counter", "2", "extra"}) ==
              RespValue::error("ERR wrong number of arguments for 'decrby' command"),
          "DECRBY excess arity");
    check(db.get("counter") == "3", "wrong argument count changed value");

    const auto Min = std::numeric_limits<std::int64_t>::min();
    const auto Max = std::numeric_limits<std::int64_t>::max();
    check(call(dispatcher, "SET", {"top", std::to_string(Max)}) == RespValue::simple_string("OK"),
          "MAX setup");
    check(call(dispatcher, "INCR", {"top"}) == overflow, "MAX+1 must overflow");
    check(call(dispatcher, "INCRBY", {"top", "1"}) == overflow, "MAX INCRBY overflow");
    check(db.get("top") == std::to_string(Max), "overflow corrupted MAX");
    check(call(dispatcher, "DECR", {"top"}) == number(Max - 1), "MAX-1");
    check(call(dispatcher, "SET", {"bottom", std::to_string(Min)}) ==
              RespValue::simple_string("OK"),
          "MIN setup");
    check(call(dispatcher, "DECR", {"bottom"}) == overflow, "MIN-1 must overflow");
    check(call(dispatcher, "DECRBY", {"bottom", "1"}) == overflow, "MIN DECRBY overflow");
    check(db.get("bottom") == std::to_string(Min), "overflow corrupted MIN");
    check(call(dispatcher, "INCR", {"bottom"}) == number(Min + 1), "MIN+1");

    check(call(dispatcher, "INCRBY", {"neg-boundary", std::to_string(Min)}) == number(Min),
          "INCRBY INT64_MIN on zero");
    check(call(dispatcher, "DECRBY", {"neg-boundary", std::to_string(Min)}) == number(0),
          "DECRBY INT64_MIN without negation");
    check(call(dispatcher, "DECRBY", {"missing-neg-min", std::to_string(Min)}) == overflow,
          "DECRBY INT64_MIN missing must overflow");
    check(!db.exists("missing-neg-min"), "overflow created missing key");
    check(call(dispatcher, "SET", {"almost", "-1"}) == RespValue::simple_string("OK"),
          "almost setup");
    check(call(dispatcher, "DECRBY", {"almost", std::to_string(Min)}) == number(Max),
          "-1 minus INT64_MIN must yield INT64_MAX");
    check(call(dispatcher, "INCRBY", {"almost", "1"}) == overflow,
          "overflow after previous successful mutation");

    for (int i = 0; i < 1600; ++i) {
        const std::string key = "loop-" + std::to_string(i % 29);
        auto original = db.get(key);
        const auto before = original ? *redisdb::storage::parse_integer64(*original) : 0;
        check(call(dispatcher, "INCRBY", {key, "7"}) == number(before + 7), "loop INCRBY failed");
        check(call(dispatcher, "DECRBY", {key, "7"}) == number(before), "loop DECRBY failed");
    }
    Database other;
    const CommandDispatcher isolated(redisdb::commands::string_routes(other));
    check(call(isolated, "INCR", {"counter"}) == number(1), "database independence broken");
    check(db.get("counter") == "3", "independent DB changed original");
}
} // namespace

int main() {
    try {
        test_integer_commands();
        std::cout
            << "PASS: Phase 4F signed integer operations, bounds, invalid input, atomic state\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
