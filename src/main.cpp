#include "redisdb/server/RespCommandServer.hpp"
#include "redisdb/server/SmokeServer.hpp"
#include "redisdb/server/Version.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <system_error>

namespace {

void print_usage() {
    std::cout << "Redis-Compatible In-Memory Database Server\n"
              << "\n"
              << "Usage:\n"
              << "  redisdb-server --version\n"
              << "  redisdb-server --help\n"
              << "  redisdb-server --smoke-echo [--port PORT]"
                 " [--max-clients COUNT]\n"
              << "  redisdb-server --resp2-server [--port PORT]"
                 " [--max-clients COUNT] [--maxmemory BYTES]"
                 " [--maxmemory-policy noeviction|allkeys-lru]\n"
              << "    [--appendonly yes] [--aof-file PATH] [--appendfsync always|everysec|no]\n"
              << "    [--snapshot-dir PATH] (requires AOF; SIGUSR1 requests a checkpoint)\n"
              << "  maxmemory: logical data charge, 0 = unlimited (default); not RSS.\n"
              << "\n"
              << "Smoke mode is for transport validation only.\n"
              << "It is NOT a Redis-compatible command server.\n"
              << "\n"
              << "Smoke defaults:\n"
              << "  Address:     127.0.0.1\n"
              << "  Port:        6380 (0 selects an ephemeral port)\n"
              << "  Max clients: 1024\n";
}

template <typename Integer> bool parse_unsigned(std::string_view text, Integer& result) {
    if (text.empty()) {
        return false;
    }

    const auto* begin = text.data();
    const auto* end = begin + text.size();

    const auto parsed = std::from_chars(begin, end, result);

    return parsed.ec == std::errc{} && parsed.ptr == end;
}

int invalid_arguments() {
    std::cerr << "Invalid arguments. Use --help for usage.\n";
    return 2;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view argument{argv[1]};

        if (argument == "--version") {
            std::cout << "redisdb-server " << redisdb::server::version() << '\n';

            return 0;
        }

        if (argument == "--help") {
            print_usage();
            return 0;
        }
    }

    if (argc < 2) {
        return invalid_arguments();
    }
    const std::string_view mode{argv[1]};
    if (mode != "--smoke-echo" && mode != "--resp2-server") {
        return invalid_arguments();
    }

    std::uint16_t port = 6380;
    std::size_t max_clients = 1024;

    bool port_seen = false;
    bool clients_seen = false;
    bool memory_seen = false;
    bool policy_seen = false;
    redisdb::storage::MemoryOptions memory;
    redisdb::persistence::AofOptions aof;
    bool append_seen = false;
    bool file_seen = false;
    bool sync_seen = false;
    redisdb::persistence::SnapshotOptions snapshot;
    bool snapshot_seen = false;

    for (int index = 2; index < argc; index += 2) {
        if (index + 1 >= argc) {
            return invalid_arguments();
        }

        const std::string_view option{argv[index]};
        const std::string_view value{argv[index + 1]};

        if (option == "--port" && !port_seen) {
            unsigned int parsed = 0;

            if (!parse_unsigned(value, parsed) ||
                parsed > std::numeric_limits<std::uint16_t>::max()) {
                return invalid_arguments();
            }

            port = static_cast<std::uint16_t>(parsed);
            port_seen = true;
            continue;
        }

        if (option == "--max-clients" && !clients_seen) {
            std::size_t parsed = 0;

            if (!parse_unsigned(value, parsed) || parsed == 0) {
                return invalid_arguments();
            }

            max_clients = parsed;
            clients_seen = true;
            continue;
        }

        if (mode == "--resp2-server" && option == "--maxmemory" && !memory_seen) {
            std::size_t parsed = 0;
            if (!parse_unsigned(value, parsed)) {
                return invalid_arguments();
            }
            if (parsed != 0) {
                memory.max_bytes = parsed;
            }
            memory_seen = true;
            continue;
        }
        if (mode == "--resp2-server" && option == "--maxmemory-policy" && !policy_seen) {
            if (value == "noeviction") {
                memory.policy = redisdb::storage::EvictionPolicy::NoEviction;
            } else if (value == "allkeys-lru") {
                memory.policy = redisdb::storage::EvictionPolicy::AllKeysLru;
            } else {
                return invalid_arguments();
            }
            policy_seen = true;
            continue;
        }
        if (mode == "--resp2-server" && option == "--appendonly" && !append_seen) {
            if (value != "yes" && value != "no") {
                return invalid_arguments();
            }
            aof.enabled = value == "yes";
            append_seen = true;
            continue;
        }
        if (mode == "--resp2-server" && option == "--aof-file" && !file_seen) {
            if (value.empty()) {
                return invalid_arguments();
            }
            aof.file = value;
            file_seen = true;
            continue;
        }
        if (mode == "--resp2-server" && option == "--appendfsync" && !sync_seen) {
            if (value == "always") {
                aof.sync = redisdb::persistence::SyncPolicy::Always;
            } else if (value == "everysec") {
                aof.sync = redisdb::persistence::SyncPolicy::EverySecond;
            } else if (value == "no") {
                aof.sync = redisdb::persistence::SyncPolicy::No;
            } else {
                return invalid_arguments();
            }
            sync_seen = true;
            continue;
        }
        if (mode == "--resp2-server" && option == "--snapshot-dir" && !snapshot_seen) {
            if (value.empty()) {
                return invalid_arguments();
            }
            snapshot.directory = value;
            snapshot_seen = true;
            continue;
        }
        return invalid_arguments();
    }

    if (!aof.enabled && (file_seen || sync_seen || snapshot_seen)) {
        return invalid_arguments();
    }

    try {
        if (mode == "--resp2-server") {
            redisdb::server::run_resp2_server(port, max_clients, memory, aof, snapshot);
        } else {
            redisdb::server::run_smoke_echo(port, max_clients);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Server error: " << error.what() << '\n';

        return 1;
    }
}
