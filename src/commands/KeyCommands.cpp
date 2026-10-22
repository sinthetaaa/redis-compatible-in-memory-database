#include "redisdb/commands/KeyCommands.hpp"

#include "redisdb/commands/ArityValidator.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstdint>

namespace redisdb::commands {
namespace {

using protocol::RespValue;

HandlerResult del_command(storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{1, std::nullopt, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'del' command")};
    }

    std::int64_t removed = 0;
    for (const auto& key : command.arguments()) {
        if (database.erase(key)) {
            ++removed; // Repeated key names count only once for DEL.
        }
    }
    return {RespValue::integer(removed)};
}

HandlerResult exists_command(const storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{1, std::nullopt, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'exists' command")};
    }

    std::int64_t present = 0;
    for (const auto& key : command.arguments()) {
        if (database.exists(key)) {
            ++present; // Redis counts repeated existing key arguments.
        }
    }
    return {RespValue::integer(present)};
}

HandlerResult type_command(const storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{1, 1, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'type' command")};
    }

    // Phase 4 supports only string values. Phase 6 expands this query
    // to inspect the variant when collection types are introduced.
    return {RespValue::simple_string(database.exists(command.arguments()[0]) ? "string" : "none")};
}

} // namespace

std::vector<CommandRoute> key_routes(storage::Database& database) {
    return {
        {"DEL", [&database](const Command& command) { return del_command(database, command); }},
        {"EXISTS",
         [&database](const Command& command) { return exists_command(database, command); }},
        {"TYPE", [&database](const Command& command) { return type_command(database, command); }},
    };
}

} // namespace redisdb::commands
