#include "redisdb/server/Operations.hpp"
#include "redisdb/server/Version.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace redisdb::server {
void add_counter(std::uint64_t& counter, std::uint64_t amount) noexcept {
    counter += std::min(amount, std::numeric_limits<std::uint64_t>::max() - counter);
}
namespace {
std::string lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
    return value;
}
// Intentionally narrow pattern subset: exact names or one trailing '*'.
bool matches(std::string_view pattern, std::string_view name) {
    return pattern == name || (!pattern.empty() && pattern.back() == '*' &&
                               name.starts_with(pattern.substr(0, pattern.size() - 1)));
}
} // namespace
protocol::RespValue operational_reply(const commands::Command& command,
                                      const OperationalView& view) {
    using protocol::RespValue;
    const auto& args = command.arguments();
    if (command.name() == "CONFIG") {
        if (args.empty() || lower(args[0]) != "get")
            return RespValue::error("ERR unsupported CONFIG subcommand; only GET is supported");
        if (args.size() < 2)
            return RespValue::error("ERR wrong number of arguments for 'config|get' command");
        std::vector<RespValue> values;
        for (const auto& [name, value] : view.settings) {
            bool selected = false;
            for (std::size_t i = 1; i < args.size(); ++i)
                selected |= matches(lower(args[i]), name);
            if (selected) {
                values.push_back(RespValue::bulk_string(name));
                values.push_back(RespValue::bulk_string(value));
            }
        }
        return RespValue::array(std::move(values));
    }
    if (args.size() > 1)
        return RespValue::error("ERR wrong number of arguments for 'info' command");
    const auto section = args.empty() ? "default" : lower(args[0]);
    const bool all = section == "all" || section == "default";
    std::string result;
    auto field = [&](std::string_view key, auto value) {
        result += std::string(key) + ":" + std::to_string(value) + "\r\n";
    };
    if (all || section == "server") {
        result += "# Server\r\nredisdb_version:" + std::string(version()) + "\r\n";
    }
    if (all || section == "clients") {
        result += "# Clients\r\n";
        field("connected_clients", view.clients);
    }
    if (all || section == "stats") {
        result += "# Stats\r\n";
        field("total_connections_received", view.stats.accepted);
        field("total_commands_processed", view.stats.commands);
        field("redisdb_commands_succeeded", view.stats.succeeded);
        field("redisdb_commands_rejected", view.stats.rejected);
        field("total_net_input_bytes", view.stats.received);
        field("total_net_output_bytes", view.stats.sent);
        field("redisdb_connections_closed", view.stats.closed);
        field("redisdb_parser_errors", view.stats.parser_errors);
        field("redisdb_transport_errors", view.stats.io_errors);
    }
    if (all || section == "memory") {
        result += "# Memory\r\n";
        field("redisdb_logical_memory_bytes", view.logical_bytes);
    }
    if (all || section == "persistence") {
        result += "# Persistence\r\n";
        field("aof_enabled", view.aof_enabled ? 1 : 0);
        field("redisdb_aof_sequence", view.aof_sequence);
    }
    // Like Redis, an unknown section is an empty bulk string.
    return RespValue::bulk_string(std::move(result));
}

void log_event(std::string_view severity, std::string_view event,
               std::string_view detail) noexcept {
    // Callers use literals only, so no arbitrary payload/path can enter JSON.
    const int written = std::fprintf(
        stderr, "{\"severity\":\"%.*s\",\"event\":\"%.*s\",\"detail\":\"%.*s\"}\n",
        static_cast<int>(severity.size()), severity.data(), static_cast<int>(event.size()),
        event.data(), static_cast<int>(detail.size()), detail.empty() ? "" : detail.data());
    if (written < 0 || std::fflush(stderr) != 0)
        std::_Exit(74);
}
void log_failure(std::string_view message) noexcept {
    // Preserve useful established diagnostic categories, never exception text
    // containing a filename, key or other user-controlled data.
    if (message.find("OOM") != std::string_view::npos) {
        log_event("error", "server_failure", "OOM maxmemory");
        return;
    }
    for (const auto category :
         {"exclusive writer lock", "AOF write", "commit watermark", "regular file", "OOM",
          "maxmemory", "snapshot write", "bind(IPv4)", "listen", "fsync", "AOF", "snapshot"}) {
        if (message.find(category) != std::string_view::npos) {
            log_event("error", "server_failure", category);
            return;
        }
    }
    log_event("error", "server_failure", "operation failed");
}
} // namespace redisdb::server
