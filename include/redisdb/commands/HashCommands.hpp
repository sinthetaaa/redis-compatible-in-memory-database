#pragma once

#include "redisdb/commands/CommandDispatcher.hpp"

#include <vector>

namespace redisdb::storage {
class Database;
}

namespace redisdb::commands {

// Phase 6C.2: seven bounded RESP2 routes borrow the serialized shared database.
[[nodiscard]] std::vector<CommandRoute> hash_routes(storage::Database& database);

} // namespace redisdb::commands
