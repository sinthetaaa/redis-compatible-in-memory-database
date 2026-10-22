#include "redisdb/commands/Command.hpp"
#include "redisdb/protocol/RespValue.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using redisdb::commands::Command;
using redisdb::commands::CommandDecodeError;
using redisdb::commands::CommandDecodeResult;
using redisdb::commands::decode_command;
using redisdb::protocol::RespValue;

static_assert(std::is_copy_constructible_v<Command>);
static_assert(std::is_copy_assignable_v<Command>);
static_assert(std::is_move_constructible_v<Command>);
static_assert(std::is_move_assignable_v<Command>);
static_assert(!std::is_default_constructible_v<Command>);

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

RespValue request(std::vector<RespValue> elements) { return RespValue::array(std::move(elements)); }

void check_success(const CommandDecodeResult& result) {
    check(result.error == CommandDecodeError::None, "success returned an error");
    check(result.command.has_value(), "success did not return a command");
    check(!result.command->name().empty(), "decoded command name is empty");
}

void check_failure(const RespValue& value, CommandDecodeError expected) {
    const RespValue original = value;
    const CommandDecodeResult result = decode_command(value);
    check(result.error == expected, "incorrect structural decode error");
    check(!result.command.has_value(), "failure fabricated a command");
    check(value == original, "failed decode mutated RESP input");
}

void test_zero_argument_command_and_ascii_case() {
    const RespValue value = request({RespValue::bulk_string("pInG")});
    const CommandDecodeResult result = decode_command(value);
    check_success(result);
    check(result.command->name() == "PING", "ASCII case canonicalization failed");
    check(result.command->arguments().empty(), "zero arguments were not preserved");
    check(value == request({RespValue::bulk_string("pInG")}), "decode mutated input");
}

void test_empty_argument_and_ordered_duplicates() {
    const RespValue value =
        request({RespValue::bulk_string("Echo"), RespValue::bulk_string(""),
                 RespValue::bulk_string("repeat"), RespValue::bulk_string("repeat")});
    const CommandDecodeResult result = decode_command(value);
    check_success(result);
    check(result.command->name() == "ECHO", "command name changed incorrectly");
    check(result.command->arguments() == std::vector<std::string>({"", "repeat", "repeat"}),
          "empty, ordered, or duplicate arguments were lost");
    check(value == request({RespValue::bulk_string("Echo"), RespValue::bulk_string(""),
                            RespValue::bulk_string("repeat"), RespValue::bulk_string("repeat")}),
          "decode mutated argument array");
}

void test_binary_argument_survives_source_destruction() {
    std::string key("\0\r\n", 3);
    key.push_back(static_cast<char>(0xff));
    key.push_back(static_cast<char>(0x80));
    key.push_back('a');

    CommandDecodeResult result;
    {
        RespValue value = request({RespValue::bulk_string("get"), RespValue::bulk_string(key)});
        result = decode_command(value);
        check_success(result);
        value = RespValue::null_array();
    }

    check(result.command->name() == "GET", "command name lost after source destruction");
    check(result.command->arguments().size() == 1, "binary argument count changed");
    check(result.command->arguments().front() == key, "binary bytes were corrupted");

    const Command copied = *result.command;
    Command moved = copied;
    Command assigned = std::move(moved);
    check(assigned == *result.command, "copied/moved command differs");

    const auto redecoded =
        decode_command(request({RespValue::bulk_string("GET"), RespValue::bulk_string(key)}));
    check_success(redecoded);
    check(*redecoded.command == assigned, "value equality is not stable");
}

void test_binary_verb_casing_does_not_change_high_bytes() {
    std::string verb = "pIng";
    verb.push_back(static_cast<char>(0xff));
    verb.push_back('\0');
    verb += "az";
    const auto result = decode_command(request({RespValue::bulk_string(verb)}));
    check_success(result);
    std::string expected = "PING";
    expected.push_back(static_cast<char>(0xff));
    expected.push_back('\0');
    expected += "AZ";
    check(result.command->name() == expected, "non-ASCII verb bytes changed");
}

void test_nonarray_and_empty_requests() {
    check_failure(RespValue::simple_string("PING"), CommandDecodeError::NotArray);
    check_failure(RespValue::error("ERR"), CommandDecodeError::NotArray);
    check_failure(RespValue::integer(7), CommandDecodeError::NotArray);
    check_failure(RespValue::bulk_string("PING"), CommandDecodeError::NotArray);
    check_failure(RespValue::null_bulk_string(), CommandDecodeError::NotArray);
    check_failure(RespValue::null_array(), CommandDecodeError::NotArray);
    check_failure(request({}), CommandDecodeError::EmptyArray);
}

void test_invalid_verbs() {
    check_failure(request({RespValue::bulk_string("")}), CommandDecodeError::EmptyVerb);
    check_failure(request({RespValue::simple_string("PING")}), CommandDecodeError::VerbNotBulk);
    check_failure(request({RespValue::error("PING")}), CommandDecodeError::VerbNotBulk);
    check_failure(request({RespValue::integer(1)}), CommandDecodeError::VerbNotBulk);
    check_failure(request({RespValue::null_bulk_string()}), CommandDecodeError::VerbNotBulk);
    check_failure(request({RespValue::null_array()}), CommandDecodeError::VerbNotBulk);
    check_failure(request({request({})}), CommandDecodeError::VerbNotBulk);
}

void test_nonbulk_arguments() {
    const std::vector<RespValue> invalid_arguments = {
        RespValue::simple_string("x"), RespValue::error("ERR"), RespValue::integer(1),
        RespValue::null_bulk_string(), RespValue::null_array(), request({})};
    for (const RespValue& bad : invalid_arguments) {
        check_failure(request({RespValue::bulk_string("SET"), bad}),
                      CommandDecodeError::NonBulkArgument);
        check_failure(request({RespValue::bulk_string("SET"), RespValue::bulk_string("safe"), bad}),
                      CommandDecodeError::NonBulkArgument);
    }
}

void test_all_result_invariants() {
    const CommandDecodeResult failure = decode_command(RespValue::null_array());
    check(failure.error != CommandDecodeError::None && !failure.command.has_value(),
          "failed result invariant is broken");
    const CommandDecodeResult success = decode_command(request({RespValue::bulk_string("PING")}));
    check_success(success);
}

} // namespace

int main() {
    try {
        test_zero_argument_command_and_ascii_case();
        test_empty_argument_and_ordered_duplicates();
        test_binary_argument_survives_source_destruction();
        test_binary_verb_casing_does_not_change_high_bytes();
        test_nonarray_and_empty_requests();
        test_invalid_verbs();
        test_nonbulk_arguments();
        test_all_result_invariants();
        std::cout << "Command representation unit tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Command representation unit tests failed: " << error.what() << '\n';
        return 1;
    }
}
