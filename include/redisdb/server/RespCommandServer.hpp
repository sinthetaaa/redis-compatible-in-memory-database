#pragma once

#include "redisdb/persistence/AofLog.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstddef>
#include <cstdint>

namespace redisdb::server {

// Limited RESP2 command-serving mode, bound only to 127.0.0.1.
// The optional logical data budget is unlimited by default.
void run_resp2_server(std::uint16_t port, std::size_t max_clients,
                      storage::MemoryOptions memory = {}, persistence::AofOptions aof = {});

} // namespace redisdb::server
