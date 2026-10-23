#pragma once

#include "redisdb/commands/CommandDispatcher.hpp"

#include <vector>

namespace redisdb::storage {
class Database;
}

namespace redisdb::commands {

// Phase 6B: list routes borrow the single-threaded, process-owned Database.
[[nodiscard]] std::vector<CommandRoute> list_routes(storage::Database& database);

} // namespace redisdb::commands
