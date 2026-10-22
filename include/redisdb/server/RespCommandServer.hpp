#pragma once

#include <cstddef>
#include <cstdint>

namespace redisdb::server {

// Limited RESP2 command-serving mode, bound only to 127.0.0.1.
// Preserves --smoke-echo and does not implement a database keyspace.
void run_resp2_server(std::uint16_t port, std::size_t max_clients);

} // namespace redisdb::server
