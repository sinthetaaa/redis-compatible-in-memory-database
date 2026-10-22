#include "redisdb/commands/CommandErrors.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace redisdb::commands;
using redisdb::protocol::RespValue;

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Command decode(std::string name, std::vector<std::string> arguments = {}) {
    std::vector<RespValue> elements;

    elements.push_back(RespValue::bulk_string(std::move(name)));

    for (auto& argument : arguments) {
        elements.push_back(RespValue::bulk_string(std::move(argument)));
    }

    auto result = decode_command(RespValue::array(std::move(elements)));

    check(result.command.has_value(), "command decoding failed");
    check(result.error == CommandDecodeError::None, "unexpected command decode error");

    return std::move(*result.command);
}

void expect(const std::string& name, const std::string& expected) {
    const Command command = decode(name, {"secret-argument"});
    const Command original = command;

    const auto reply = unknown_command_reply(command);

    check(reply == RespValue::error(expected), "unknown-command response mismatch");
    check(command == original, "command was modified");
}

void run_tests() {
    expect("unknown", "ERR unknown command 'UNKNOWN'");
    expect("MiXeD", "ERR unknown command 'MIXED'");

    expect(std::string("a'\r\n\\", 5), "ERR unknown command 'A\\'\\x0D\\x0A\\\\'");

    std::string binary = "a";
    binary.push_back(static_cast<char>(0xFF));

    expect(binary, "ERR unknown command 'A\\xFF'");

    expect(std::string(60, 'x'), "ERR unknown command '" + std::string(48, 'X') + "...'");

    // No request argument must appear in diagnostic output.
    const Command command = decode("MISSING", {"private-data"});
    const auto reply = unknown_command_reply(command);

    check(reply == RespValue::error("ERR unknown command 'MISSING'"),
          "unknown-command error leaked arguments");

    for (int index = 0; index < 1000; ++index) {
        expect("unregistered", "ERR unknown command 'UNREGISTERED'");
    }
}

} // namespace

int main() {
    try {
        run_tests();
        std::cout << "PASS: bounded unknown-command errors\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
