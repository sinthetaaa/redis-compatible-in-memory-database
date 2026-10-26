#pragma once

#include "redisdb/persistence/Snapshot.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace redisdb::server {

// Limited RESP2 command-serving mode; loopback unless explicitly configured.
// The optional logical data budget is unlimited by default.
void run_resp2_server(std::uint16_t port, std::size_t max_clients,
                      storage::MemoryOptions memory = {}, persistence::AofOptions aof = {},
                      persistence::SnapshotOptions snapshot = {},
                      std::string_view bind_address = "127.0.0.1");

} // namespace redisdb::server
