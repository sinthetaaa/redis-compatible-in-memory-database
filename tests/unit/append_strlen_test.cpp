#include "redisdb/commands/StringCommands.hpp"

#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/protocol/RespValue.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using redisdb::commands::CommandDispatcher;
using redisdb::commands::MaxStringValueBytes;
using redisdb::protocol::RespValue;
using redisdb::storage::Database;

namespace {
void check(bool ok, const char* reason) {
    if (!ok) {
        throw std::runtime_error(reason);
    }
}

RespValue call(const CommandDispatcher& dispatcher, std::string verb,
               std::vector<std::string> args = {}) {
    std::vector<RespValue> wire;
    wire.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& arg : args) {
        wire.push_back(RespValue::bulk_string(std::move(arg)));
    }
    auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(wire)));
    check(decoded.command.has_value(), "failed to decode command");
    auto reply = dispatcher.dispatch(*decoded.command);
    check(reply.handled.has_value(), "APPEND or STRLEN not registered");
    check(reply.handled->disposition == redisdb::commands::ConnectionDisposition::KeepOpen,
          "string command attempted to close client");
    return std::move(reply.handled->reply);
}

void run() {
    Database database;
    const CommandDispatcher dispatcher(redisdb::commands::string_routes(database));
    check(call(dispatcher, "STRLEN", {"missing"}) == RespValue::integer(0),
          "STRLEN of missing key must be zero");
    check(!database.exists("missing"), "STRLEN must not create a key");
    check(call(dispatcher, "APPEND", {"greeting", "Hello"}) == RespValue::integer(5),
          "APPEND must create missing key");
    check(call(dispatcher, "aPpEnD", {"greeting", " World"}) == RespValue::integer(11),
          "APPEND existing key length incorrect");
    check(call(dispatcher, "sTrLeN", {"greeting"}) == RespValue::integer(11),
          "STRLEN existing key mismatch");
    check(call(dispatcher, "GET", {"greeting"}) == RespValue::bulk_string("Hello World"),
          "APPEND broke GET");
    check(call(dispatcher, "APPEND", {"greeting", ""}) == RespValue::integer(11),
          "empty append should preserve existing length");

    check(call(dispatcher, "APPEND", {"", ""}) == RespValue::integer(0),
          "empty key and value must be stored");
    check(database.exists("") && database.string_length("") == 0,
          "APPEND empty key did not create an empty string");
    check(call(dispatcher, "GET", {""}) == RespValue::bulk_string(""),
          "empty string must not become nil");
    const std::string key("K\0\xff\r\n", 5);
    const std::string suffix("\0\xff\r\n", 4);
    check(call(dispatcher, "APPEND", {key, suffix}) == RespValue::integer(4),
          "binary append missing key");
    check(call(dispatcher, "APPEND", {key, suffix}) == RespValue::integer(8),
          "binary append existing key");
    check(call(dispatcher, "GET", {key}) == RespValue::bulk_string(suffix + suffix),
          "binary APPEND roundtrip");
    check(call(dispatcher, "STRLEN", {key}) == RespValue::integer(8), "STRLEN must count bytes");
    check(call(dispatcher, "STRLEN", {"k"}) == RespValue::integer(0),
          "STRLEN must keep key case-sensitive");

    const auto bad_append = RespValue::error("ERR wrong number of arguments for 'append' command");
    const auto bad_strlen = RespValue::error("ERR wrong number of arguments for 'strlen' command");
    check(call(dispatcher, "APPEND") == bad_append &&
              call(dispatcher, "APPEND", {"only-key"}) == bad_append &&
              call(dispatcher, "APPEND", {"a", "b", "c"}) == bad_append,
          "APPEND arity mismatch");
    check(call(dispatcher, "STRLEN") == bad_strlen &&
              call(dispatcher, "STRLEN", {"a", "b"}) == bad_strlen,
          "STRLEN arity mismatch");
    check(!database.exists("only-key"), "invalid APPEND mutated the keyspace");

    const std::string maximum(MaxStringValueBytes, 'x');
    const auto oversize =
        RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit");
    check(call(dispatcher, "APPEND", {"max", maximum}) ==
              RespValue::integer(static_cast<std::int64_t>(MaxStringValueBytes)),
          "maximum-size initial APPEND must succeed");
    check(call(dispatcher, "APPEND", {"max", ""}) ==
              RespValue::integer(static_cast<std::int64_t>(MaxStringValueBytes)),
          "empty suffix at limit must succeed");
    check(call(dispatcher, "APPEND", {"max", "z"}) == oversize, "APPEND past limit must error");
    check(call(dispatcher, "GET", {"max"}) == RespValue::bulk_string(maximum),
          "failed append corrupted prior value");
    check(call(dispatcher, "APPEND", {"new-too-big", maximum + "!"}) == oversize,
          "missing-key oversize APPEND must error");
    check(!database.exists("new-too-big"), "failed append created missing key");

    (void)database.set("internal-oversized", maximum + "!");
    check(call(dispatcher, "STRLEN", {"internal-oversized"}) ==
              RespValue::integer(static_cast<std::int64_t>(MaxStringValueBytes + 1)),
          "STRLEN should count internal stored bytes exactly");
    check(call(dispatcher, "APPEND", {"internal-oversized", ""}) == oversize,
          "oversize internal value must not be mutated");
    check(database.get("internal-oversized") == maximum + "!",
          "rejected oversized append mutated storage");

    // Erasure remains visible to STRLEN without adding key command routes.
    check(database.erase("greeting"), "database deletion failed");
    check(call(dispatcher, "STRLEN", {"greeting"}) == RespValue::integer(0),
          "STRLEN after deletion must be zero");

    // Deterministic model checks over 1,200 APPEND/STRLEN pairs.
    for (int i = 0; i < 1200; ++i) {
        const std::string iteration_key = "loop-" + std::to_string(i % 31);
        const std::size_t before = database.string_length(iteration_key);
        const std::string part(1, static_cast<char>(i % 256));
        check(call(dispatcher, "APPEND", {iteration_key, part}) ==
                  RespValue::integer(static_cast<std::int64_t>(before + 1)),
              "deterministic APPEND mismatch");
        check(call(dispatcher, "STRLEN", {iteration_key}) ==
                  RespValue::integer(static_cast<std::int64_t>(before + 1)),
              "deterministic STRLEN mismatch");
    }

    Database independent;
    const CommandDispatcher isolated(redisdb::commands::string_routes(independent));
    check(call(isolated, "STRLEN", {"max"}) == RespValue::integer(0),
          "separate databases leaked state");
}
} // namespace

int main() {
    try {
        run();
        std::cout << "PASS: binary-safe APPEND/STRLEN with bounded atomic updates\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
