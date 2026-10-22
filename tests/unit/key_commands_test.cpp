#include "redisdb/commands/KeyCommands.hpp"

#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/StringCommands.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using redisdb::commands::CommandDispatcher;
using redisdb::commands::ConnectionDisposition;
using redisdb::protocol::RespValue;
using redisdb::storage::Database;

namespace {

void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

RespValue call(const CommandDispatcher& dispatcher, std::string name,
               std::vector<std::string> arguments = {}) {
    std::vector<RespValue> elements;
    elements.push_back(RespValue::bulk_string(std::move(name)));
    for (auto& arg : arguments) {
        elements.push_back(RespValue::bulk_string(std::move(arg)));
    }
    auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(elements)));
    check(decoded.command.has_value(), "decode failed");
    auto outcome = dispatcher.dispatch(*decoded.command);
    check(outcome.handled.has_value(), "key route missing");
    check(outcome.handled->disposition == ConnectionDisposition::KeepOpen,
          "key commands must leave connection open");
    return std::move(outcome.handled->reply);
}

void test_behavior() {
    Database database;
    auto routes = redisdb::commands::string_routes(database);
    for (auto& route : redisdb::commands::key_routes(database)) {
        routes.push_back(std::move(route));
    }
    const CommandDispatcher dispatcher(std::move(routes));

    check(call(dispatcher, "TYPE", {"absent"}) == RespValue::simple_string("none"), "missing type");
    check(call(dispatcher, "EXISTS", {"absent", "absent"}) == RespValue::integer(0),
          "missing exists");
    check(call(dispatcher, "DEL", {"absent", "absent"}) == RespValue::integer(0), "missing del");
    check(call(dispatcher, "SET", {"a", "first"}) == RespValue::simple_string("OK"), "setup SET");
    check(call(dispatcher, "SET", {"b", "second"}) == RespValue::simple_string("OK"),
          "setup SET b");
    check(call(dispatcher, "eXiStS", {"a", "a", "b", "absent", "b"}) == RespValue::integer(4),
          "EXISTS must count each argument including repeats");
    check(call(dispatcher, "TYPE", {"a"}) == RespValue::simple_string("string"),
          "existing type must be string");
    check(call(dispatcher, "dEl", {"a", "absent", "a", "b", "b"}) == RespValue::integer(2),
          "DEL must delete distinct extant keys just once");
    check(database.size() == 0 && !database.exists("a"), "DEL storage state mismatch");
    check(call(dispatcher, "GET", {"b"}) == RespValue::null_bulk_string(),
          "deleted GET must be nil");
    check(call(dispatcher, "TYPE", {"b"}) == RespValue::simple_string("none"),
          "deleted TYPE must be none");

    const std::string binary_key("x\0\xff\r\n", 5);
    const std::string binary_value("v\0\xff", 3);
    check(call(dispatcher, "SET", {binary_key, binary_value}) == RespValue::simple_string("OK"),
          "binary setup");
    check(call(dispatcher, "EXISTS", {binary_key, "x", binary_key}) == RespValue::integer(2),
          "binary EXISTS repeat");
    check(call(dispatcher, "TYPE", {binary_key}) == RespValue::simple_string("string"),
          "binary TYPE");
    check(call(dispatcher, "DEL", {"x", binary_key, binary_key}) == RespValue::integer(1),
          "binary DEL");
    check(!database.exists(binary_key), "binary DEL did not remove exact key");

    check(call(dispatcher, "SET", {"", ""}) == RespValue::simple_string("OK"), "empty key setup");
    check(call(dispatcher, "TYPE", {""}) == RespValue::simple_string("string"), "empty key TYPE");
    check(call(dispatcher, "EXISTS", {"", ""}) == RespValue::integer(2),
          "empty key EXISTS duplicates");
    check(call(dispatcher, "DEL", {""}) == RespValue::integer(1), "empty key DEL");

    check(call(dispatcher, "DEL") ==
              RespValue::error("ERR wrong number of arguments for 'del' command"),
          "DEL arity");
    check(call(dispatcher, "EXISTS") ==
              RespValue::error("ERR wrong number of arguments for 'exists' command"),
          "EXISTS arity");
    check(call(dispatcher, "TYPE") ==
              RespValue::error("ERR wrong number of arguments for 'type' command"),
          "TYPE arity no argument");
    check(call(dispatcher, "TYPE", {"a", "b"}) ==
              RespValue::error("ERR wrong number of arguments for 'type' command"),
          "TYPE arity too many");

    for (int i = 0; i < 1000; ++i) {
        const std::string key = "round-" + std::to_string(i);
        check(call(dispatcher, "SET", {key, "data"}) == RespValue::simple_string("OK"),
              "round SET");
        check(call(dispatcher, "EXISTS", {key, key}) == RespValue::integer(2), "round EXISTS");
        check(call(dispatcher, "DEL", {key, key}) == RespValue::integer(1), "round DEL");
        check(call(dispatcher, "TYPE", {key}) == RespValue::simple_string("none"), "round TYPE");
    }
    check(database.size() == 0, "test leaked keys");

    Database independent;
    const CommandDispatcher other(redisdb::commands::key_routes(independent));
    check(call(other, "EXISTS", {"a"}) == RespValue::integer(0),
          "independent database contamination");
}

} // namespace

int main() {
    try {
        test_behavior();
        std::cout << "PASS: DEL/EXISTS/TYPE binary, repeated keys, arity, 1000 cycles\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
