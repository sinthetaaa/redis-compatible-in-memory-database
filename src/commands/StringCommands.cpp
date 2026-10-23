#include "redisdb/commands/StringCommands.hpp"

#include "redisdb/commands/ArityValidator.hpp"
#include "redisdb/storage/Database.hpp"

#include <string>
#include <utility>

namespace redisdb::commands {
namespace {

using protocol::RespValue;

HandlerResult set_command(storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{2, 2, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'set' command")};
    }

    const auto& arguments = command.arguments();
    if (arguments[1].size() > MaxStringValueBytes) {
        return {RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit")};
    }

    // Count, value limits and ownership are validated before mutation.
    // Phase 5 adds EX/PX; Phase 4B intentionally accepts no SET options.
    (void)database.set(arguments[0], arguments[1]);
    return {RespValue::simple_string("OK")};
}

HandlerResult get_command(const storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{1, 1, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'get' command")};
    }

    const auto value = database.get(command.arguments()[0]);
    if (!value.has_value()) {
        return {RespValue::null_bulk_string()};
    }

    // Defensive guard for future storage writers added after Phase 4B.
    if (value->size() > MaxStringValueBytes) {
        return {RespValue::error("ERR stored string exceeds the 8176-byte reply-safe limit")};
    }

    return {RespValue::bulk_string(*value)};
}

HandlerResult mset_command(storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{2, std::nullopt, 2}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'mset' command")};
    }

    const auto& args = command.arguments();
    // All validation occurs before any storage modification.
    for (std::size_t index = 1; index < args.size(); index += 2) {
        if (args[index].size() > MaxStringValueBytes) {
            return {RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit")};
        }
    }
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(args.size() / 2);
    for (std::size_t index = 0; index < args.size(); index += 2) {
        pairs.emplace_back(args[index], args[index + 1]);
    }
    database.set_many(pairs);
    return {RespValue::simple_string("OK")};
}

HandlerResult mget_command(const storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{1, std::nullopt, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'mget' command")};
    }

    constexpr std::size_t MaxReplyBytes = 8 * 1024;
    const auto& keys = command.arguments();
    std::size_t written = 3 + std::to_string(keys.size()).size(); // *N\r\n
    std::vector<RespValue> values;
    values.reserve(keys.size());
    for (const auto& key : keys) {
        auto value = database.get(key);
        std::size_t additional = 5; // $-1\r\n for missing keys
        if (value) {
            // Check before framing arithmetic or allocation: stored values
            // can also be introduced through future non-SET command paths.
            if (value->size() > MaxReplyBytes) {
                return {RespValue::error("ERR MGET response exceeds the 8192-byte limit")};
            }
            // '$' + decimal count + CRLF + bytes + CRLF.
            additional = 5 + std::to_string(value->size()).size() + value->size();
        }
        if (additional > MaxReplyBytes - written) {
            return {RespValue::error("ERR MGET response exceeds the 8192-byte limit")};
        }
        written += additional;
        if (value) {
            values.push_back(RespValue::bulk_string(std::move(*value)));
        } else {
            values.push_back(RespValue::null_bulk_string());
        }
    }
    return {RespValue::array(std::move(values))};
}

} // namespace

std::vector<CommandRoute> string_routes(storage::Database& database) {
    return {
        {"SET", [&database](const Command& command) { return set_command(database, command); }},
        {"GET", [&database](const Command& command) { return get_command(database, command); }},
        {"MSET", [&database](const Command& command) { return mset_command(database, command); }},
        {"MGET", [&database](const Command& command) { return mget_command(database, command); }},
    };
}

} // namespace redisdb::commands
