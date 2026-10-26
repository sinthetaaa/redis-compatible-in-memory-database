#include "redisdb/server/Operations.hpp"
#include "redisdb/server/Version.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

using redisdb::protocol::RespValue;
using redisdb::server::OperationalView;

void check(bool value) {
    if (!value)
        throw std::runtime_error("operational response mismatch");
}
RespValue reply(OperationalView& view, std::vector<std::string> args) {
    std::vector<RespValue> parts;
    for (auto& arg : args)
        parts.push_back(RespValue::bulk_string(std::move(arg)));
    return redisdb::server::operational_reply(
        *redisdb::commands::decode_command(RespValue::array(std::move(parts))).command, view);
}
int main() {
    try {
        const auto child = ::fork();
        check(child >= 0);
        if (child == 0) {
            ::close(STDERR_FILENO);
            redisdb::server::log_event("info", "test");
            std::_Exit(1);
        }
        int status = 0;
        check(::waitpid(child, &status, 0) == child);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 74);
        std::uint64_t counter = std::numeric_limits<std::uint64_t>::max() - 1;
        redisdb::server::add_counter(counter, 10);
        redisdb::server::add_counter(counter);
        check(counter == std::numeric_limits<std::uint64_t>::max());
        OperationalView view;
        view.clients = 3;
        view.logical_bytes = 123;
        view.aof_enabled = true;
        view.aof_sequence = 8;
        view.settings = {{"port", "6380"}, {"maxmemory", "0"}};
        check(reply(view, {"INFO", "clients"}) ==
              RespValue::bulk_string("# Clients\r\nconnected_clients:3\r\n"));
        check(reply(view, {"INFO", "memory"}) ==
              RespValue::bulk_string("# Memory\r\nredisdb_logical_memory_bytes:123\r\n"));
        check(
            reply(view, {"INFO", "PERSISTENCE"}) ==
            RespValue::bulk_string("# Persistence\r\naof_enabled:1\r\nredisdb_aof_sequence:8\r\n"));
        check(reply(view, {"INFO", "server"}) ==
              RespValue::bulk_string("# Server\r\nredisdb_version:" +
                                     std::string(redisdb::server::version()) + "\r\n"));
        check(reply(view, {"INFO", "missing"}) == RespValue::bulk_string(""));
        check(reply(view, {"INFO", "a", "b"}) ==
              RespValue::error("ERR wrong number of arguments for 'info' command"));
        check(reply(view, {"CONFIG", "GET", "PORT"}) ==
              RespValue::array({RespValue::bulk_string("port"), RespValue::bulk_string("6380")}));
        check(reply(view, {"CONFIG", "get", "*", "port"}) ==
              RespValue::array({RespValue::bulk_string("port"), RespValue::bulk_string("6380"),
                                RespValue::bulk_string("maxmemory"), RespValue::bulk_string("0")}));
        check(reply(view, {"CONFIG", "GET", "max*"}) ==
              RespValue::array({RespValue::bulk_string("maxmemory"), RespValue::bulk_string("0")}));
        check(reply(view, {"CONFIG", "GET", "?ort"}) == RespValue::array({}));
        check(reply(view, {"CONFIG", "GET", "missing"}) == RespValue::array({}));
        check(reply(view, {"CONFIG", "GET"}) ==
              RespValue::error("ERR wrong number of arguments for 'config|get' command"));
        check(reply(view, {"CONFIG", "SET", "port", "1"}) ==
              RespValue::error("ERR unsupported CONFIG subcommand; only GET is supported"));
        std::cout << "PASS operational wire values and saturating counters\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
