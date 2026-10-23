#pragma once

#include "redisdb/commands/CommandDispatcher.hpp"

#include <vector>

namespace redisdb::storage {
class Database;
}

namespace redisdb::commands {

// Phase 6D.2: six bounded RESP2 set routes borrow the shared serialized database.
[[nodiscard]] std::vector<CommandRoute> set_routes(storage::Database& database);

} // namespace redisdb::commands
