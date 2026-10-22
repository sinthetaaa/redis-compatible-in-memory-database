#pragma once

#include "redisdb/commands/CommandDispatcher.hpp"

#include <vector>

namespace redisdb::storage {
class Database;
}

namespace redisdb::commands {

// Key commands use the server-owned Database shared by all TCP clients.
// The single event-loop thread serializes access; route closures borrow the
// database, which must outlive the dispatcher.
[[nodiscard]] std::vector<CommandRoute> key_routes(storage::Database& database);

} // namespace redisdb::commands
