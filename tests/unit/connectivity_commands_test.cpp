#include "redisdb/commands/ConnectivityCommands.hpp"
#include "redisdb/protocol/RespValue.hpp"

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

    std::vector<RespValue> values;
    values.push_back(RespValue::bulk_string(std::move(name)));

    for (auto& argument : arguments) {
        values.push_back(RespValue::bulk_string(std::move(argument)));
    }

    auto result = decode_command(RespValue::array(std::move(values)));

    check(result.command.has_value(), "decode failed");
    check(result.error == CommandDecodeError::None, "unexpected decode error");

    return std::move(*result.command);
}

void expect(const CommandDispatcher& dispatcher, std::string name,
            std::vector<std::string> arguments, const RespValue& expected,
            ConnectionDisposition disposition = ConnectionDisposition::KeepOpen) {

    const Command command = decode(std::move(name), std::move(arguments));
    const Command original = command;

    const auto result = dispatcher.dispatch(command);

    check(result.error == DispatchError::None, "unexpected dispatch error");
    check(result.handled.has_value(), "handler result missing");
    check(result.handled->reply == expected, "incorrect RESP2 reply");
    check(result.handled->disposition == disposition, "incorrect connection disposition");
    check(command == original, "handler modified input command");
}

void test_ping(const CommandDispatcher& dispatcher) {
    expect(dispatcher, "PING", {}, RespValue::simple_string("PONG"));

    expect(dispatcher, "pInG", {"hello"}, RespValue::bulk_string("hello"));

    expect(dispatcher, "PING", {""}, RespValue::bulk_string(""));

    expect(dispatcher, "PING", {"one", "two"},
           RespValue::error("ERR wrong number of arguments for 'ping' command"));
}

void test_echo(const CommandDispatcher& dispatcher) {
    expect(dispatcher, "ECHO", {"hello"}, RespValue::bulk_string("hello"));

    expect(dispatcher, "ECHO", {""}, RespValue::bulk_string(""));

    expect(dispatcher, "ECHO", {},
           RespValue::error("ERR wrong number of arguments for 'echo' command"));

    expect(dispatcher, "ECHO", {"one", "two"},
           RespValue::error("ERR wrong number of arguments for 'echo' command"));
}

void test_binary_data(const CommandDispatcher& dispatcher) {
    std::string binary("A\0\r\n", 4);
    binary.push_back(static_cast<char>(0x80));
    binary.push_back(static_cast<char>(0xff));

    expect(dispatcher, "PING", {binary}, RespValue::bulk_string(binary));

    expect(dispatcher, "ECHO", {binary}, RespValue::bulk_string(binary));

    std::string large(4096, 'x');

    expect(dispatcher, "ECHO", {large}, RespValue::bulk_string(large));
}

void test_quit(const CommandDispatcher& dispatcher) {
    expect(dispatcher, "QUIT", {}, RespValue::simple_string("OK"),
           ConnectionDisposition::CloseAfterReply);

    expect(dispatcher, "qUiT", {"unexpected"},
           RespValue::error("ERR wrong number of arguments for 'quit' command"));

    expect(dispatcher, "QUIT", {"one", "two"},
           RespValue::error("ERR wrong number of arguments for 'quit' command"));
}

void test_unknown_and_repeat(const CommandDispatcher& dispatcher) {

    const auto unknown = dispatcher.dispatch(decode("NOT_REGISTERED"));

    check(unknown.error == DispatchError::UnknownCommand, "unknown-command semantics changed");
    check(!unknown.handled.has_value(), "unknown command fabricated a response");

    for (int i = 0; i < 100; ++i) {
        expect(dispatcher, "PING", {}, RespValue::simple_string("PONG"));

        expect(dispatcher, "ECHO", {"repeat"}, RespValue::bulk_string("repeat"));

        expect(dispatcher, "QUIT", {}, RespValue::simple_string("OK"),
               ConnectionDisposition::CloseAfterReply);
    }
}

} // namespace

int main() {
    try {
        const CommandDispatcher dispatcher(connectivity_routes());

        test_ping(dispatcher);
        test_echo(dispatcher);
        test_binary_data(dispatcher);
        test_quit(dispatcher);
        test_unknown_and_repeat(dispatcher);

        std::cout << "PASS: PING ECHO QUIT connectivity handlers\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
