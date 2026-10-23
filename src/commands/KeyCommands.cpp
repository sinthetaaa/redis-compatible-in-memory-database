#include "redisdb/commands/KeyCommands.hpp"

#include "redisdb/commands/ArityValidator.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

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

HandlerResult expire_command(storage::Database& database, const Command& command, bool milliseconds,
                             const char* name) {
    if (!validate_arity(command, ArityRule{2, 2, 1}).valid()) {
        return {RespValue::error(std::string("ERR wrong number of arguments for '") + name +
                                 "' command")};
    }
    const auto duration = storage::parse_integer64(command.arguments()[1]);
    if (!duration.has_value()) {
        return {RespValue::error("ERR value is not an integer or out of range")};
    }
    switch (database.expire_after(command.arguments()[0], *duration, milliseconds)) {
    case storage::ExpireResult::Missing:
        return {RespValue::integer(0)};
    case storage::ExpireResult::Set:
    case storage::ExpireResult::Deleted:
        return {RespValue::integer(1)};
    case storage::ExpireResult::OutOfRange:
        return {RespValue::error(std::string("ERR invalid expire time in '") + name + "' command")};
    }
    return {RespValue::error("ERR unexpected expiration outcome")};
}

HandlerResult ttl_command(const storage::Database& database, const Command& command,
                          bool milliseconds, const char* name) {
    if (!validate_arity(command, ArityRule{1, 1, 1}).valid()) {
        return {RespValue::error(std::string("ERR wrong number of arguments for '") + name +
                                 "' command")};
    }
    return {RespValue::integer(database.remaining_ttl(command.arguments()[0], milliseconds))};
}

HandlerResult persist_command(storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{1, 1, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'persist' command")};
    }
    // The metadata API already handles an expired key as absent, atomically.
    return {RespValue::integer(database.clear_expiration(command.arguments()[0]) ? 1 : 0)};
}

HandlerResult type_command(const storage::Database& database, const Command& command) {
    if (!validate_arity(command, ArityRule{1, 1, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'type' command")};
    }

    switch (database.type_of(command.arguments()[0])) {
    case storage::StoredType::None:
        return {RespValue::simple_string("none")};
    case storage::StoredType::String:
        return {RespValue::simple_string("string")};
    case storage::StoredType::List:
        return {RespValue::simple_string("list")};
    case storage::StoredType::Hash:
        return {RespValue::simple_string("hash")};
    case storage::StoredType::Set:
        return {RespValue::simple_string("set")};
    }
    throw std::logic_error("unexpected stored type");
}

} // namespace

std::vector<CommandRoute> key_routes(storage::Database& database) {
    return {
        {"DEL", [&database](const Command& command) { return del_command(database, command); }},
        {"EXISTS",
         [&database](const Command& command) { return exists_command(database, command); }},
        {"TYPE", [&database](const Command& command) { return type_command(database, command); }},
        {"EXPIRE",
         [&database](const Command& command) {
             return expire_command(database, command, false, "expire");
         }},
        {"PEXPIRE",
         [&database](const Command& command) {
             return expire_command(database, command, true, "pexpire");
         }},
        {"TTL",
         [&database](const Command& command) {
             return ttl_command(database, command, false, "ttl");
         }},
        {"PTTL",
         [&database](const Command& command) {
             return ttl_command(database, command, true, "pttl");
         }},
        {"PERSIST",
         [&database](const Command& command) { return persist_command(database, command); }},
    };
}

} // namespace redisdb::commands
