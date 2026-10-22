#pragma once

#include "redisdb/commands/CommandDispatcher.hpp"

#include <cstddef>
#include <vector>

namespace redisdb::storage {
class Database;
}

namespace redisdb::commands {

// The default RESP2 pipeline limits each serialized reply to 8192 bytes.
// Reserve 16 bytes for RESP2 bulk-string framing; reject larger SETs
// before mutation so every accepted string remains GET-readable.
inline constexpr std::size_t MaxStringValueBytes = 8192 - 16;

// Stateless route objects whose handlers borrow the process-level database.
// The owning command server must keep Database alive past the dispatcher.
[[nodiscard]] std::vector<CommandRoute> string_routes(storage::Database& database);

} // namespace redisdb::commands
