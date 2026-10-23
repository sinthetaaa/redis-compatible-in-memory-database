#include "redisdb/commands/StringCommands.hpp"

#include "redisdb/commands/ArityValidator.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace redisdb::commands {
namespace {

using protocol::RespValue;

HandlerResult set_command(storage::Database& database, const Command& command) {
    const auto& arguments = command.arguments();
    // Preserve the existing mandatory-two-argument form, and accept exactly
    // one relative EX/PX option pair. NX/XX/GET/KEEPTTL remain unsupported.
    if (arguments.size() != 2 && arguments.size() != 4) {
        return {RespValue::error("ERR wrong number of arguments for 'set' command")};
    }
    if (arguments[1].size() > MaxStringValueBytes) {
        return {RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit")};
    }
    if (arguments.size() == 2) {
        (void)database.set(arguments[0], arguments[1]);
        return {RespValue::simple_string("OK")};
    }

    // Parse options as ASCII bytes, independent of locale and binary keys.
    const std::string_view option = arguments[2];
    const bool seconds = option.size() == 2 && (option[0] == 'E' || option[0] == 'e') &&
                         (option[1] == 'X' || option[1] == 'x');
    const bool milliseconds = option.size() == 2 && (option[0] == 'P' || option[0] == 'p') &&
                              (option[1] == 'X' || option[1] == 'x');
    if (!seconds && !milliseconds) {
        return {RespValue::error("ERR syntax error")};
    }
    const auto amount = storage::parse_integer64(arguments[3]);
    if (!amount.has_value()) {
        return {RespValue::error("ERR value is not an integer or out of range")};
    }
    // Redis SET rejects non-positive timeouts, unlike EXPIRE's delete-now.
    if (*amount <= 0 ||
        database.set_with_expiration(arguments[0], arguments[1], *amount, milliseconds) ==
            storage::SetWithExpiryResult::OutOfRange) {
        return {RespValue::error("ERR invalid expire time in 'set' command")};
    }
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

HandlerResult append_command(storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{2, 2, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'append' command")};
    }

    const auto& args = command.arguments();
    const auto length = database.append(args[0], args[1], MaxStringValueBytes);
    if (!length.has_value()) {
        // Existing bytes and missing-key state are preserved on rejection.
        return {RespValue::error("ERR string value exceeds the 8176-byte reply-safe limit")};
    }
    return {RespValue::integer(static_cast<std::int64_t>(*length))};
}

HandlerResult strlen_command(const storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{1, 1, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'strlen' command")};
    }
    return {RespValue::integer(
        static_cast<std::int64_t>(database.string_length(command.arguments()[0])))};
}

HandlerResult integer_command(storage::Database& database, const Command& command, bool subtract,
                              bool with_operand, const char* name) {
    const std::size_t required = with_operand ? 2 : 1;
    if (!validate_arity(command, ArityRule{required, required, 1}).valid()) {
        return {RespValue::error(std::string("ERR wrong number of arguments for '") + name +
                                 "' command")};
    }
    std::int64_t operand = 1;
    if (with_operand) {
        const auto parsed = storage::parse_integer64(command.arguments()[1]);
        if (!parsed.has_value()) {
            return {RespValue::error("ERR value is not an integer or out of range")};
        }
        operand = *parsed;
    }

    const auto result = database.adjust_integer(command.arguments()[0], operand, subtract);
    switch (result.error) {
    case storage::IntegerMutationError::None:
        return {RespValue::integer(result.value)};
    case storage::IntegerMutationError::InvalidInteger:
        return {RespValue::error("ERR value is not an integer or out of range")};
    case storage::IntegerMutationError::Overflow:
        return {RespValue::error("ERR increment or decrement would overflow")};
    }
    throw std::logic_error("invalid integer mutation error");
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
        {"APPEND",
         [&database](const Command& command) { return append_command(database, command); }},
        {"STRLEN",
         [&database](const Command& command) { return strlen_command(database, command); }},
        {"INCR",
         [&database](const Command& command) {
             return integer_command(database, command, false, false, "incr");
         }},
        {"DECR",
         [&database](const Command& command) {
             return integer_command(database, command, true, false, "decr");
         }},
        {"INCRBY",
         [&database](const Command& command) {
             return integer_command(database, command, false, true, "incrby");
         }},
        {"DECRBY",
         [&database](const Command& command) {
             return integer_command(database, command, true, true, "decrby");
         }},
    };
}

} // namespace redisdb::commands
