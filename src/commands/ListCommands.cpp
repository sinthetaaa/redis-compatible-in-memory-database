#include "redisdb/commands/ListCommands.hpp"

#include "redisdb/commands/ArityValidator.hpp"
#include "redisdb/commands/StringCommands.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace redisdb::commands {
namespace {
using protocol::RespValue;
constexpr std::size_t MaxReplyBytes = 8192;

HandlerResult push(storage::Database& db, const Command& command, bool left, const char* name) {
    if (!validate_arity(command, ArityRule{2, std::nullopt, 1}).valid()) {
        return {RespValue::error(std::string("ERR wrong number of arguments for '") + name +
                                 "' command")};
    }
    const auto& args = command.arguments();
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i].size() > MaxStringValueBytes) {
            return {RespValue::error("ERR list element exceeds the 8176-byte reply-safe limit")};
        }
    }
    std::vector<std::string> values(args.begin() + 1, args.end());
    try {
        const auto prior = db.list_length(args[0]);
        const auto max_length = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
        if (prior > max_length || values.size() > max_length - prior) {
            return {RespValue::error("ERR list length exceeds supported range")};
        }
        const auto count = db.list_push(args[0], values, left);
        return {RespValue::integer(static_cast<std::int64_t>(count))};
    } catch (const storage::WrongTypeError& error) {
        return {RespValue::error(error.what())};
    }
}

HandlerResult length(storage::Database& db, const Command& command) {
    if (!validate_arity(command, ArityRule{1, 1, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'llen' command")};
    }
    try {
        const auto count = db.list_length(command.arguments()[0]);
        return {RespValue::integer(static_cast<std::int64_t>(count))};
    } catch (const storage::WrongTypeError& error) {
        return {RespValue::error(error.what())};
    }
}

HandlerResult pop(storage::Database& db, const Command& command, bool left, const char* name) {
    if (!validate_arity(command, ArityRule{1, 1, 1}).valid()) {
        return {RespValue::error(std::string("ERR wrong number of arguments for '") + name +
                                 "' command")};
    }
    try {
        const auto item = db.list_pop(command.arguments()[0], left);
        if (!item) {
            return {RespValue::null_bulk_string()};
        }
        // All RESP2 list writes reject values exceeding MaxStringValueBytes.
        return {RespValue::bulk_string(std::move(*item))};
    } catch (const storage::WrongTypeError& error) {
        return {RespValue::error(error.what())};
    }
}

HandlerResult range(storage::Database& db, const Command& command) {
    if (!validate_arity(command, ArityRule{3, 3, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'lrange' command")};
    }
    const auto& args = command.arguments();
    const auto start = storage::parse_integer64(args[1]);
    const auto stop = storage::parse_integer64(args[2]);
    if (!start || !stop) {
        return {RespValue::error("ERR value is not an integer or out of range")};
    }
    try {
        auto items = db.list_range(args[0], *start, *stop, MaxReplyBytes);
        if (!items) {
            return {RespValue::error("ERR LRANGE response exceeds the 8192-byte limit")};
        }
        std::vector<RespValue> reply;
        reply.reserve(items->size());
        for (auto& value : *items) {
            reply.push_back(RespValue::bulk_string(std::move(value)));
        }
        return {RespValue::array(std::move(reply))};
    } catch (const storage::WrongTypeError& error) {
        return {RespValue::error(error.what())};
    }
}
} // namespace

std::vector<CommandRoute> list_routes(storage::Database& db) {
    return {
        {"LPUSH", [&db](const Command& c) { return push(db, c, true, "lpush"); }},
        {"RPUSH", [&db](const Command& c) { return push(db, c, false, "rpush"); }},
        {"LLEN", [&db](const Command& c) { return length(db, c); }},
        {"LPOP", [&db](const Command& c) { return pop(db, c, true, "lpop"); }},
        {"RPOP", [&db](const Command& c) { return pop(db, c, false, "rpop"); }},
        {"LRANGE", [&db](const Command& c) { return range(db, c); }},
    };
}

} // namespace redisdb::commands
