#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/commands/KeyCommands.hpp"
#include "redisdb/commands/StringCommands.hpp"
#include "redisdb/protocol/RespValue.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using redisdb::commands::CommandDispatcher;
using redisdb::protocol::RespValue;
using redisdb::storage::Database;

namespace {
void check(bool valid, const char* what) {
    if (!valid) {
        throw std::runtime_error(what);
    }
}

RespValue call(const CommandDispatcher& dispatcher, std::string verb,
               std::vector<std::string> arguments = {}) {
    std::vector<RespValue> encoded;
    encoded.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& item : arguments) {
        encoded.push_back(RespValue::bulk_string(std::move(item)));
    }
    auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(encoded)));
    check(decoded.command.has_value(), "request decoding failed");
    auto result = dispatcher.dispatch(*decoded.command);
    check(result.handled.has_value(), "composite route missing");
    check(result.handled->disposition == redisdb::commands::ConnectionDisposition::KeepOpen,
          "unexpected connection disposition");
    return std::move(result.handled->reply);
}

void test_shared_dispatcher_model() {
    Database database;
    auto routes = redisdb::commands::string_routes(database);
    for (auto& route : redisdb::commands::key_routes(database)) {
        routes.push_back(std::move(route));
    }
    const CommandDispatcher dispatcher(std::move(routes));
    std::unordered_map<std::string, std::string> expected;
    const std::vector<std::string> keys = {"alpha",   "BETA", "",   std::string("bin\0\xff\r\n", 7),
                                           "alpha:2", "late", "xyz"};
    const std::string missing = "definitely-never-set";
    const auto nil = RespValue::null_bulk_string();
    const auto bulk_or_nil = [&](const std::string& key) {
        const auto it = expected.find(key);
        return it == expected.end() ? nil : RespValue::bulk_string(it->second);
    };

    for (int iteration = 0; iteration < 2400; ++iteration) {
        const int turn = iteration / 12;
        const std::string& key = keys[static_cast<std::size_t>(turn) % keys.size()];
        const std::string& other = keys[(static_cast<std::size_t>(turn) + 3) % keys.size()];
        const std::string counter = "counter:" + std::to_string(turn % 5);
        switch (iteration % 12) {
        case 0: {
            const auto value = "v-" + std::to_string(turn);
            check(call(dispatcher, "SET", {key, value}) == RespValue::simple_string("OK"),
                  "model SET failed");
            expected[key] = value;
            break;
        }
        case 1:
            check(call(dispatcher, "GET", {key}) == bulk_or_nil(key), "model GET mismatch");
            break;
        case 2: {
            const std::string suffix("\0z", 2);
            const std::size_t length = expected[key].size() + suffix.size();
            check(call(dispatcher, "APPEND", {key, suffix}) ==
                      RespValue::integer(static_cast<std::int64_t>(length)),
                  "model APPEND length mismatch");
            expected[key] += suffix;
            break;
        }
        case 3:
            check(call(dispatcher, "STRLEN", {key}) ==
                      RespValue::integer(static_cast<std::int64_t>(expected.at(key).size())),
                  "model STRLEN mismatch");
            break;
        case 4: {
            const std::int64_t before =
                expected.contains(counter) ? std::stoll(expected.at(counter)) : 0;
            check(call(dispatcher, "INCR", {counter}) == RespValue::integer(before + 1),
                  "model shared counter INCR mismatch");
            expected[counter] = std::to_string(before + 1);
            break;
        }
        case 5: {
            const std::int64_t before = std::stoll(expected.at(counter));
            check(call(dispatcher, "DECRBY", {counter, "2"}) == RespValue::integer(before - 2),
                  "model shared counter DECRBY mismatch");
            expected[counter] = std::to_string(before - 2);
            break;
        }
        case 6: {
            const std::string winner = "last-" + std::to_string(turn);
            const std::string side = "side-" + std::to_string(turn);
            check(call(dispatcher, "MSET", {key, "discarded", other, side, key, winner}) ==
                      RespValue::simple_string("OK"),
                  "model MSET mismatch");
            expected[key] = winner;
            expected[other] = side;
            break;
        }
        case 7:
            check(
                call(dispatcher, "MGET", {key, other, missing, key}) ==
                    RespValue::array({bulk_or_nil(key), bulk_or_nil(other), nil, bulk_or_nil(key)}),
                "model MGET ordering or snapshot mismatch");
            break;
        case 8: {
            const std::int64_t present = static_cast<std::int64_t>(expected.contains(key)) * 2 +
                                         static_cast<std::int64_t>(expected.contains(other));
            check(call(dispatcher, "EXISTS", {key, key, other, missing}) ==
                      RespValue::integer(present),
                  "model EXISTS duplicates mismatch");
            break;
        }
        case 9:
            check(call(dispatcher, "TYPE", {key}) ==
                      RespValue::simple_string(expected.contains(key) ? "string" : "none"),
                  "model TYPE mismatch");
            break;
        case 10: {
            const auto removed = static_cast<std::int64_t>(expected.erase(key));
            check(call(dispatcher, "DEL", {key, key, missing}) == RespValue::integer(removed),
                  "model DEL duplicate accounting mismatch");
            break;
        }
        case 11:
            check(call(dispatcher, "GET", {counter}) == bulk_or_nil(counter),
                  "cross-command integer storage visibility mismatch");
            break;
        default:
            throw std::logic_error("impossible operation");
        }
    }
    check(database.size() == expected.size(), "model final key count mismatch");
    for (const auto& [key, value] : expected) {
        check(database.get(key) == value, "model final key/value snapshot mismatch");
    }
    Database isolated;
    const CommandDispatcher second(redisdb::commands::string_routes(isolated));
    check(call(second, "GET", {"counter:0"}) == nil, "independent keyspaces leaked");
}
} // namespace

int main() {
    try {
        test_shared_dispatcher_model();
        std::cout << "PASS: 4H 2400 model-checked shared string/key commands and atomic batches\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
