#pragma once

#include <cstddef>
#include <cstdint>

namespace redisdb::server {

// Explicit transport-only test mode.
//
// Binds to 127.0.0.1, runs a native readiness loop,
// echoes received bytes, and exits on SIGINT/SIGTERM.
//
// This is not a Redis protocol implementation.
void run_smoke_echo(std::uint16_t port, std::size_t max_clients);

} // namespace redisdb::server
