#pragma once

#include "redisdb/commands/Command.hpp"
#include "redisdb/protocol/RespValue.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redisdb::server {

// Event-loop-owned counters saturate at UINT64_MAX; reset on process restart.
void add_counter(std::uint64_t& counter, std::uint64_t amount = 1) noexcept;
struct RuntimeStats {
    std::uint64_t commands = 0, succeeded = 0, rejected = 0, parser_errors = 0;
    std::uint64_t accepted = 0, closed = 0, received = 0, sent = 0, io_errors = 0;
};
struct OperationalView {
    RuntimeStats stats;
    std::size_t clients = 0, logical_bytes = 0;
    bool aof_enabled = false;
    std::uint64_t aof_sequence = 0;
    std::vector<std::pair<std::string, std::string>> settings;
};
// Called only for INFO or CONFIG, before any database clock/transaction scope.
[[nodiscard]] protocol::RespValue operational_reply(const commands::Command& command,
                                                    const OperationalView& view);
// Only constant, non-sensitive identifiers are accepted by callers. Failure to
// write/flush diagnostics terminates with exit code 74, without recursive logging.
void log_event(std::string_view severity, std::string_view event,
               std::string_view detail = {}) noexcept;
void log_failure(std::string_view message) noexcept;

} // namespace redisdb::server
