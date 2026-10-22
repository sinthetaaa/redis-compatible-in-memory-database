#include "redisdb/commands/StringCommands.hpp"

#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/ConnectivityCommands.hpp"
#include "redisdb/storage/Database.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using redisdb::commands::Command;
using redisdb::commands::CommandDispatcher;
using redisdb::commands::ConnectionDisposition;
using redisdb::commands::HandlerResult;
using redisdb::commands::MaxStringValueBytes;
using redisdb::protocol::RespValue;
using redisdb::storage::Database;

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

HandlerResult dispatch(const CommandDispatcher& dispatcher, std::string verb,
                       std::vector<std::string> args = {}) {
    std::vector<RespValue> values;
    values.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& arg : args) {
        values.push_back(RespValue::bulk_string(std::move(arg)));
    }
    auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(values)));
    check(decoded.command.has_value(), "command decoder failed");
    auto result = dispatcher.dispatch(*decoded.command);
    check(result.handled.has_value(), "string command not registered");
    check(result.handled->disposition == ConnectionDisposition::KeepOpen,
          "string commands must not close clients");
    return std::move(*result.handled);
}

void test_set_get() {
    Database database;
    auto routes = redisdb::commands::string_routes(database);
    const CommandDispatcher dispatcher(std::move(routes));
    const auto& d = dispatcher;

    check(dispatch(d, "get", {"missing"}).reply == RespValue::null_bulk_string(),
          "missing GET must return nil bulk");
    check(dispatch(d, "set", {"user:1", "Alice"}).reply == RespValue::simple_string("OK"),
          "initial SET must return OK");
    check(dispatch(d, "GET", {"user:1"}).reply == RespValue::bulk_string("Alice"),
          "GET first value");
    check(dispatch(d, "SeT", {"user:1", "Bob"}).reply == RespValue::simple_string("OK"),
          "overwrite must succeed");
    check(dispatch(d, "gEt", {"user:1"}).reply == RespValue::bulk_string("Bob"),
          "GET after overwrite");
    check(database.size() == 1, "overwrite must not change key count");

    const std::string binary_key("a\0\r\n", 4);
    const std::string binary_value("\0\xff\r\n", 4);
    check(dispatch(d, "SET", {binary_key, binary_value}).reply == RespValue::simple_string("OK"),
          "binary SET");
    check(dispatch(d, "GET", {binary_key}).reply == RespValue::bulk_string(binary_value),
          "binary GET");
    check(dispatch(d, "SET", {"", ""}).reply == RespValue::simple_string("OK"),
          "empty key/value SET");
    check(dispatch(d, "GET", {""}).reply == RespValue::bulk_string(""),
          "empty value differs from nil");
    check(dispatch(d, "GET", {"USER:1"}).reply == RespValue::null_bulk_string(),
          "key lookup must be case-sensitive");

    check(dispatch(d, "SET", {"user:1"}).reply ==
              RespValue::error("ERR wrong number of arguments for 'set' command"),
          "too few SET args");
    check(dispatch(d, "SET", {"user:1", "broken", "NX"}).reply ==
              RespValue::error("ERR wrong number of arguments for 'set' command"),
          "unsupported SET option must not mutate");
    check(dispatch(d, "GET", {}).reply ==
              RespValue::error("ERR wrong number of arguments for 'get' command"),
          "too few GET args");
    check(dispatch(d, "GET", {"user:1", "bad"}).reply ==
              RespValue::error("ERR wrong number of arguments for 'get' command"),
          "too many GET args");
    check(database.get("user:1") == "Bob", "invalid command mutated database");

    const std::string max_value(MaxStringValueBytes, 'z');
    check(dispatch(d, "SET", {"boundary", max_value}).reply == RespValue::simple_string("OK"),
          "maximum value must be accepted");
    check(dispatch(d, "GET", {"boundary"}).reply == RespValue::bulk_string(max_value),
          "maximum value must round trip");
    check(dispatch(d, "SET", {"boundary", max_value + "!"}).reply ==
              RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit"),
          "oversize overwrite must fail");
    check(database.get("boundary") == max_value, "failed SET must preserve previous value");
    check(dispatch(d, "SET", {"new-oversize", max_value + "!"}).reply ==
              RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit"),
          "oversize new value must fail");
    check(!database.exists("new-oversize"), "failed SET created a key");

    // A direct internal future writer can insert a larger value. GET must
    // return a RESP2 error rather than overflowing serializer / closing TCP.
    (void)database.set("legacy-oversize", max_value + "!");
    check(dispatch(d, "GET", {"legacy-oversize"}).reply ==
              RespValue::error("ERR stored string exceeds the 8176-byte reply-safe limit"),
          "oversize internal value must produce a bounded error");

    // SET and GET must not borrow the caller's data or cross DB instances.
    Database other;
    const CommandDispatcher other_dispatcher(redisdb::commands::string_routes(other));
    const auto& d2 = other_dispatcher;
    check(dispatch(d2, "GET", {"user:1"}).reply == RespValue::null_bulk_string(),
          "separate databases leaked state");

    for (int i = 0; i < 1000; ++i) {
        std::string key = "key-" + std::to_string(i % 37);
        std::string value = "value-" + std::to_string(i);
        check(dispatch(d, "SET", {key, value}).reply == RespValue::simple_string("OK"),
              "repeated SET failed");
        check(dispatch(d, "GET", {key}).reply == RespValue::bulk_string(value),
              "repeated GET failed");
    }
}

} // namespace

int main() {
    try {
        test_set_get();
        std::cout << "PASS: SET/GET 1000 operation pairs, binary keys, arity, bounds, ownership\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
