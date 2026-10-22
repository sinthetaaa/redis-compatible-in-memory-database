#include "redisdb/commands/StringCommands.hpp"

#include "redisdb/commands/ArityValidator.hpp"
#include "redisdb/storage/Database.hpp"

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

} // namespace

std::vector<CommandRoute> string_routes(storage::Database& database) {
    return {
        {"SET", [&database](const Command& command) { return set_command(database, command); }},
        {"GET", [&database](const Command& command) { return get_command(database, command); }},
    };
}

} // namespace redisdb::commands
