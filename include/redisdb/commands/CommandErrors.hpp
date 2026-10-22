#pragma once

#include "redisdb/commands/Command.hpp"
#include "redisdb/protocol/RespValue.hpp"

namespace redisdb::commands {

// Bounded, ASCII-safe RESP2 error for unknown commands.
// Does not reveal argument contents.
[[nodiscard]] protocol::RespValue unknown_command_reply(const Command& command);

} // namespace redisdb::commands
