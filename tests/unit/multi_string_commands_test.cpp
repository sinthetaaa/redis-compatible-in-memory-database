#include "redisdb/commands/StringCommands.hpp"

#include "redisdb/commands/Command.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/protocol/RespValue.hpp"
#include "redisdb/storage/Database.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using redisdb::commands::CommandDispatcher;
using redisdb::protocol::RespValue;
using redisdb::storage::Database;

namespace {
void check(bool condition, const char* reason) {
    if (!condition)
        throw std::runtime_error(reason);
}

RespValue call(const CommandDispatcher& d, std::string name, std::vector<std::string> args = {}) {
    std::vector<RespValue> elements;
    elements.push_back(RespValue::bulk_string(std::move(name)));
    for (auto& arg : args)
        elements.push_back(RespValue::bulk_string(std::move(arg)));
    auto decoded = redisdb::commands::decode_command(RespValue::array(std::move(elements)));
    check(decoded.command.has_value(), "decode failed");
    auto out = d.dispatch(*decoded.command);
    check(out.handled.has_value(), "unregistered command");
    check(out.handled->disposition == redisdb::commands::ConnectionDisposition::KeepOpen,
          "command closed connection");
    return std::move(out.handled->reply);
}

RespValue array(std::vector<RespValue> elements) { return RespValue::array(std::move(elements)); }

void tests() {
    Database db;
    const CommandDispatcher d(redisdb::commands::string_routes(db));
    const auto nil = RespValue::null_bulk_string();
    check(call(d, "MGET", {"a", "a", "missing"}) == array({nil, nil, nil}),
          "all missing / repeated MGET");
    check(call(d, "mSeT", {"a", "one", "b", "two"}) == RespValue::simple_string("OK"),
          "initial MSET");
    check(call(d, "MGET", {"b", "a", "absent", "a"}) ==
              array({RespValue::bulk_string("two"), RespValue::bulk_string("one"), nil,
                     RespValue::bulk_string("one")}),
          "ordered MGET, nil and duplicates");
    check(call(d, "MSET", {"a", "first", "c", "z", "a", "last"}) == RespValue::simple_string("OK"),
          "duplicate MSET accepted");
    check(db.size() == 3 && db.get("a") == "last", "last MSET occurrence must win");

    const std::string binary_key("k\0\xff\r\n", 5);
    const std::string binary_value("v\0\xfe\r\n", 5);
    check(call(d, "MSET", {binary_key, binary_value, "", ""}) == RespValue::simple_string("OK"),
          "binary and empty keys");
    check(call(d, "MGET", {binary_key, "", "missing"}) ==
              array({RespValue::bulk_string(binary_value), RespValue::bulk_string(""), nil}),
          "binary roundtrip and empty versus nil");
    check(call(d, "MGET", {"K", "a"}) == array({nil, RespValue::bulk_string("last")}),
          "keys are case-sensitive");

    const auto wrong_mset = RespValue::error("ERR wrong number of arguments for 'mset' command");
    const auto wrong_mget = RespValue::error("ERR wrong number of arguments for 'mget' command");
    check(call(d, "MSET", {}) == wrong_mset && call(d, "MSET", {"orphan"}) == wrong_mset &&
              call(d, "MSET", {"a", "bad", "dangling"}) == wrong_mset,
          "MSET count/group validation");
    check(call(d, "MGET", {}) == wrong_mget, "MGET empty rejected");
    check(db.get("a") == "last" && !db.exists("orphan"), "invalid count mutated DB");

    const std::string max_value(redisdb::commands::MaxStringValueBytes, 'z');
    const auto oversized =
        RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit");
    check(call(d, "MSET", {"a", "should-not-change", "bad", max_value + "!"}) == oversized,
          "late oversized pair rejected");
    check(db.get("a") == "last" && !db.exists("bad"), "failed MSET partially mutated DB");
    check(call(d, "MSET", {"max", max_value}) == RespValue::simple_string("OK"),
          "valid maximal value accepted");
    check(call(d, "MGET", {"max"}) == array({RespValue::bulk_string(max_value)}),
          "single maximal value fits MGET framing");
    const auto too_big = RespValue::error("ERR MGET response exceeds the 8192-byte limit");
    check(call(d, "MGET", {"max", "max"}) == too_big,
          "aggregate limit must reject duplicated maximum value");
    check(call(d, "GET", {"max"}) == RespValue::bulk_string(max_value),
          "rejected MGET must not corrupt state");
    check(call(d, "MGET", {"a"}) == array({RespValue::bulk_string("last")}),
          "command still usable after overflow response");

    (void)db.set("internal-big", max_value + std::string(32, '!'));
    check(call(d, "MGET", {"internal-big"}) == too_big, "internal oversized value safe error");

    // Model agreement, including repeated keys and order, after 200 batches.
    for (int i = 0; i < 200; ++i) {
        const std::string key = "batch-" + std::to_string(i % 19);
        const std::string value = "v-" + std::to_string(i);
        check(call(d, "MSET", {key, "old", key, value}) == RespValue::simple_string("OK"),
              "batch repeated key failed");
        check(call(d, "MGET", {key, key, "never"}) ==
                  array({RespValue::bulk_string(value), RespValue::bulk_string(value), nil}),
              "batch value/order mismatch");
    }

    // Direct database batches stage replacement before swapping authoritative state.
    db.set_many({{"a", "new"}, {"a", "final"}, {"new", "owned"}});
    check(db.get("a") == "final" && db.get("new") == "owned", "storage batch semantics");
    Database other;
    const CommandDispatcher other_routes(redisdb::commands::string_routes(other));
    check(call(other_routes, "MGET", {"a"}) == array({nil}), "independent databases leaked");
}
} // namespace

int main() {
    try {
        tests();
        std::cout << "PASS: atomic MSET and ordered MGET, binary, bounds, 200 batches\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
