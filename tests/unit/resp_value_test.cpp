#include "redisdb/protocol/RespValue.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using redisdb::protocol::Array;
using redisdb::protocol::BulkString;
using redisdb::protocol::Error;
using redisdb::protocol::Integer;
using redisdb::protocol::NullArray;
using redisdb::protocol::NullBulkString;
using redisdb::protocol::RespValue;
using redisdb::protocol::SimpleString;

static_assert(std::is_copy_constructible_v<RespValue>);
static_assert(std::is_move_constructible_v<RespValue>);
static_assert(std::is_copy_assignable_v<RespValue>);
static_assert(std::is_move_assignable_v<RespValue>);

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Action> void expect_invalid_argument(Action&& action, const char* message) {
    bool caught = false;

    try {
        std::forward<Action>(action)();
    } catch (const std::invalid_argument&) {
        caught = true;
    }

    check(caught, message);
}

void test_simple_strings_and_errors() {
    const auto ok = RespValue::simple_string("OK");
    const auto err = RespValue::error("ERR invalid command");

    check(std::holds_alternative<SimpleString>(ok.storage()), "simple string type incorrect");

    check(std::get<SimpleString>(ok.storage()).text == "OK", "simple string content incorrect");

    check(std::holds_alternative<Error>(err.storage()), "error type incorrect");

    check(std::get<Error>(err.storage()).message == "ERR invalid command",
          "error content incorrect");

    check(!(ok == RespValue::error("OK")), "simple string and error confused");

    check(RespValue::simple_string("") == RespValue::simple_string(""),
          "empty simple string rejected");

    check(RespValue::error("") == RespValue::error(""), "empty error rejected");

    expect_invalid_argument([] { (void)RespValue::simple_string("a\rb"); },
                            "CR accepted in simple string");

    expect_invalid_argument([] { (void)RespValue::simple_string("a\nb"); },
                            "LF accepted in simple string");

    expect_invalid_argument([] { (void)RespValue::error("x\r"); }, "CR accepted in error");

    expect_invalid_argument([] { (void)RespValue::error("x\n"); }, "LF accepted in error");
}

void test_integers() {
    for (const std::int64_t value : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
                                     std::int64_t{0}, std::numeric_limits<std::int64_t>::max()}) {
        const auto integer = RespValue::integer(value);

        check(std::holds_alternative<Integer>(integer.storage()), "integer type incorrect");

        check(std::get<Integer>(integer.storage()).value == value, "integer value incorrect");

        check(integer == RespValue::integer(value), "integer equality incorrect");
    }
}

void test_bulk_and_null_values() {
    std::string bytes("A\0B\r\n", 5);
    bytes.push_back(static_cast<char>(0xff));

    const auto payload = RespValue::bulk_string(bytes);

    check(std::holds_alternative<BulkString>(payload.storage()), "bulk type incorrect");

    check(std::get<BulkString>(payload.storage()).bytes == bytes, "binary bytes were changed");

    check(std::get<BulkString>(payload.storage()).bytes.size() == 6, "binary byte count incorrect");

    const auto empty_bulk = RespValue::bulk_string("");
    const auto null_bulk = RespValue::null_bulk_string();
    const auto empty_array = RespValue::array({});
    const auto null_array = RespValue::null_array();

    check(std::holds_alternative<BulkString>(empty_bulk.storage()), "empty bulk type incorrect");

    check(std::holds_alternative<NullBulkString>(null_bulk.storage()), "null bulk type incorrect");

    check(std::holds_alternative<Array>(empty_array.storage()), "empty array type incorrect");

    check(std::holds_alternative<NullArray>(null_array.storage()), "null array type incorrect");

    check(!(empty_bulk == null_bulk), "empty bulk and null bulk confused");

    check(!(empty_array == null_array), "empty array and null array confused");

    check(!(null_bulk == null_array), "null bulk and null array confused");
}

void test_nested_arrays_and_copy_ownership() {
    const auto original = RespValue::array(
        {RespValue::simple_string("OK"), RespValue::integer(-42),
         RespValue::array({RespValue::bulk_string(std::string("x\0y", 3)),
                           RespValue::null_bulk_string(), RespValue::null_array()}),
         RespValue::array({})});

    const RespValue copied = original;

    check(copied == original, "nested array equality incorrect");

    const auto& original_items = std::get<Array>(original.storage()).elements;

    const auto& copied_items = std::get<Array>(copied.storage()).elements;

    check(original_items.size() == 4, "outer array size incorrect");

    check(&original_items[0] != &copied_items[0], "copy shares array storage");

    check(std::holds_alternative<NullArray>(
              std::get<Array>(copied_items[2].storage()).elements[2].storage()),
          "nested null array lost");

    RespValue reassigned = RespValue::integer(1);
    reassigned = original;

    check(reassigned == original, "copy assignment incorrect");

    RespValue moved = std::move(reassigned);

    check(moved == original, "move construction incorrect");

    RespValue move_assigned = RespValue::null_array();
    move_assigned = std::move(moved);

    check(move_assigned == original, "move assignment incorrect");

    check(!(original == RespValue::array({RespValue::integer(1)})),
          "different arrays compared equal");
}

void test_nested_binary_buffer_ownership() {
    // Force heap-backed string storage rather than
    // relying on implementation-specific SSO behavior.
    std::string binary(160, 'Q');

    binary[17] = '\0';
    binary[63] = '\r';
    binary[64] = '\n';
    binary[159] = static_cast<char>(0xff);

    const RespValue original = RespValue::array(
        {RespValue::array({RespValue::bulk_string(binary), RespValue::null_array()}),
         RespValue::null_bulk_string()});

    const RespValue copied = original;

    check(copied == original, "deeply nested copied value differs");

    const auto& original_outer = std::get<Array>(original.storage()).elements;

    const auto& copied_outer = std::get<Array>(copied.storage()).elements;

    check(original_outer.size() == 2 && copied_outer.size() == 2, "outer array structure changed");

    const auto& original_inner = std::get<Array>(original_outer.at(0).storage()).elements;

    const auto& copied_inner = std::get<Array>(copied_outer.at(0).storage()).elements;

    check(original_inner.size() == 2 && copied_inner.size() == 2, "nested array structure changed");

    const auto& original_bytes = std::get<BulkString>(original_inner.at(0).storage()).bytes;

    const auto& copied_bytes = std::get<BulkString>(copied_inner.at(0).storage()).bytes;

    check(original_bytes == binary && copied_bytes == binary, "nested binary contents changed");

    check(original_bytes.size() == 160 && copied_bytes.size() == 160,
          "nested binary payload length changed");

    check(original_bytes.data() != copied_bytes.data(),
          "deep copy shares nested bulk-string storage");

    check(std::holds_alternative<NullArray>(copied_inner.at(1).storage()),
          "nested null array changed");

    check(std::holds_alternative<NullBulkString>(copied_outer.at(1).storage()),
          "outer null bulk changed");
}

} // namespace

int main() {
    try {
        test_simple_strings_and_errors();
        test_integers();
        test_bulk_and_null_values();
        test_nested_arrays_and_copy_ownership();
        test_nested_binary_buffer_ownership();

        std::cout << "RESP2 value-model tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RESP2 value-model test failed: " << error.what() << '\n';
        return 1;
    }
}
