#pragma once

#include "redisdb/commands/CommandDispatcher.hpp"

#include <vector>

namespace redisdb::commands {

// Stateless built-in routes. No sockets or global registry.
[[nodiscard]] std::vector<CommandRoute> connectivity_routes();

} // namespace redisdb::commands
