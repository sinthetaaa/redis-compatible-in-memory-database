#include "redisdb/commands/ConnectivityCommands.hpp"

#include "redisdb/commands/ArityValidator.hpp"

namespace redisdb::commands {
namespace {

using protocol::RespValue;

HandlerResult ping(const Command& command) {
    if (!validate_arity(command, ArityRule{0, 1, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'ping' command")};
    }

    if (command.arguments().empty()) {
        return {RespValue::simple_string("PONG")};
    }

    return {RespValue::bulk_string(command.arguments().front())};
}

HandlerResult echo(const Command& command) {
    if (!validate_arity(command, ArityRule{1, 1, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'echo' command")};
    }

    return {RespValue::bulk_string(command.arguments().front())};
}

HandlerResult quit(const Command& command) {
    if (!validate_arity(command, ArityRule{0, 0, 1}).valid()) {
        return {RespValue::error("ERR wrong number of arguments for 'quit' command")};
    }

    return {RespValue::simple_string("OK"), ConnectionDisposition::CloseAfterReply};
}

} // namespace

std::vector<CommandRoute> connectivity_routes() {
    return {
        {"PING", ping},
        {"ECHO", echo},
        {"QUIT", quit},
    };
}

} // namespace redisdb::commands
