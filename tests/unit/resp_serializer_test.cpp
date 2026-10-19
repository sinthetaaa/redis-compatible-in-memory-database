#include "redisdb/protocol/RespSerializer.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using redisdb::protocol::RespSerializer;
using redisdb::protocol::RespValue;

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_bytes(const RespSerializer& serializer, const RespValue& value,
                 std::string_view expected, const char* message) {
    const std::string actual = serializer.serialize(value);

    check(actual.size() == expected.size(), message);
    check(actual == expected, message);
}

template <typename Error, typename Action>
void expect_exception(Action&& action, const char* message) {
    bool caught = false;

    try {
        std::forward<Action>(action)();
    } catch (const Error&) {
        caught = true;
    }

    check(caught, message);
}

void test_simple_strings_and_errors() {
    const RespSerializer serializer;

    check_bytes(serializer, RespValue::simple_string("OK"), "+OK\r\n",
                "simple string framing incorrect");

    check_bytes(serializer, RespValue::simple_string(""), "+\r\n",
                "empty simple string framing incorrect");

    check_bytes(serializer, RespValue::error("ERR unknown command"), "-ERR unknown command\r\n",
                "error framing incorrect");

    check_bytes(serializer, RespValue::error(""), "-\r\n", "empty error framing incorrect");
}

void test_signed_integers() {
    const RespSerializer serializer;

    check_bytes(serializer, RespValue::integer(0), ":0\r\n", "zero integer incorrect");

    check_bytes(serializer, RespValue::integer(-1), ":-1\r\n", "negative integer incorrect");

    check_bytes(serializer, RespValue::integer(123456789), ":123456789\r\n",
                "positive integer incorrect");

    check_bytes(serializer, RespValue::integer(std::numeric_limits<std::int64_t>::min()),
                ":-9223372036854775808\r\n", "minimum signed integer incorrect");

    check_bytes(serializer, RespValue::integer(std::numeric_limits<std::int64_t>::max()),
                ":9223372036854775807\r\n", "maximum signed integer incorrect");
}

void test_binary_safe_bulk_strings() {
    const RespSerializer serializer;

    check_bytes(serializer, RespValue::bulk_string("hello"), "$5\r\nhello\r\n",
                "bulk string framing incorrect");

    check_bytes(serializer, RespValue::bulk_string(""), "$0\r\n\r\n",
                "empty bulk string incorrect");

    std::string binary("A\0B\r\n", 5);
    binary.push_back(static_cast<char>(0xff));

    std::string expected("$6\r\n");
    expected.append(binary);
    expected.append("\r\n");

    check_bytes(serializer, RespValue::bulk_string(binary), expected,
                "binary bulk string bytes changed");

    check(serializer.serialize(RespValue::bulk_string(binary)).size() == 12,
          "binary bulk frame length incorrect");
}

void test_null_representations() {
    const RespSerializer serializer;

    check_bytes(serializer, RespValue::null_bulk_string(), "$-1\r\n", "null bulk incorrect");

    check_bytes(serializer, RespValue::null_array(), "*-1\r\n", "null array incorrect");

    check_bytes(serializer, RespValue::array({}), "*0\r\n", "empty array incorrect");

    check_bytes(serializer, RespValue::bulk_string(""), "$0\r\n\r\n",
                "empty bulk confused with null bulk");
}

void test_nested_arrays() {
    const RespSerializer serializer;

    const RespValue value = RespValue::array(
        {RespValue::simple_string("OK"), RespValue::integer(-42),
         RespValue::array({RespValue::bulk_string("foo"), RespValue::null_bulk_string(),
                           RespValue::null_array()}),
         RespValue::array({})});

    const std::string expected = "*4\r\n"
                                 "+OK\r\n"
                                 ":-42\r\n"
                                 "*3\r\n"
                                 "$3\r\nfoo\r\n"
                                 "$-1\r\n"
                                 "*-1\r\n"
                                 "*0\r\n";

    check_bytes(serializer, value, expected, "nested mixed-type array incorrect");

    check_bytes(serializer,
                RespValue::array({RespValue::array({RespValue::array({RespValue::integer(7)})})}),
                "*1\r\n*1\r\n*1\r\n:7\r\n", "three-level nested array incorrect");
}

void test_output_limit_exact_boundary() {
    const RespSerializer exact(RespSerializer::Limits{7, 10, 5});

    check_bytes(exact, RespValue::simple_string("abcd"), "+abcd\r\n",
                "exact output boundary rejected");

    const RespSerializer too_small(RespSerializer::Limits{6, 10, 5});

    expect_exception<std::length_error>(
        [&] { (void)too_small.serialize(RespValue::simple_string("abcd")); },
        "oversized simple string accepted");

    const RespSerializer bulk_limit(RespSerializer::Limits{8, 10, 5});

    expect_exception<std::length_error>(
        [&] { (void)bulk_limit.serialize(RespValue::bulk_string("abc")); },
        "oversized bulk string accepted");

    check_bytes(RespSerializer(RespSerializer::Limits{9, 10, 5}), RespValue::bulk_string("abc"),
                "$3\r\nabc\r\n", "exact bulk boundary rejected");
}

void test_array_element_limits() {
    const RespSerializer limited(RespSerializer::Limits{1024, 2, 5});

    check_bytes(limited, RespValue::array({RespValue::integer(1), RespValue::integer(2)}),
                "*2\r\n:1\r\n:2\r\n", "array at element limit rejected");

    expect_exception<std::length_error>(
        [&] {
            (void)limited.serialize(RespValue::array(
                {RespValue::integer(1), RespValue::integer(2), RespValue::integer(3)}));
        },
        "oversized array accepted");

    expect_exception<std::length_error>(
        [&] {
            (void)limited.serialize(RespValue::array({RespValue::array(
                {RespValue::integer(1), RespValue::integer(2), RespValue::integer(3)})}));
        },
        "nested oversized array accepted");
}

void test_nesting_depth_limits() {
    const RespSerializer one_level(RespSerializer::Limits{1024, 10, 1});

    check_bytes(one_level, RespValue::array({RespValue::integer(1), RespValue::null_array()}),
                "*2\r\n:1\r\n*-1\r\n", "one-level array incorrectly rejected");

    expect_exception<std::length_error>(
        [&] {
            (void)one_level.serialize(
                RespValue::array({RespValue::array({RespValue::integer(1)})}));
        },
        "nested array exceeded depth limit");

    const RespSerializer three_levels(RespSerializer::Limits{1024, 10, 3});

    check_bytes(three_levels,
                RespValue::array({RespValue::array({RespValue::array({RespValue::integer(1)})})}),
                "*1\r\n*1\r\n*1\r\n:1\r\n", "array at depth limit rejected");
}

void test_cumulative_output_limit() {
    const RespValue combined =
        RespValue::array({RespValue::bulk_string("abc"), RespValue::bulk_string("def")});

    // *2 CRLF = 4 bytes.
    // Each $3 CRLF abc CRLF = 9 bytes.
    // Total = 22 bytes.
    const RespSerializer exact(RespSerializer::Limits{22, 10, 5});

    check_bytes(exact, combined, "*2\r\n$3\r\nabc\r\n$3\r\ndef\r\n",
                "cumulative exact size incorrect");

    const RespSerializer too_small(RespSerializer::Limits{21, 10, 5});

    expect_exception<std::length_error>([&] { (void)too_small.serialize(combined); },
                                        "cumulative response limit not enforced");
}

void test_invalid_configurations() {
    expect_exception<std::invalid_argument>(
        [] { (void)RespSerializer(RespSerializer::Limits{0, 10, 5}); },
        "zero output limit accepted");

    expect_exception<std::invalid_argument>(
        [] { (void)RespSerializer(RespSerializer::Limits{1024, 0, 5}); },
        "zero element limit accepted");

    expect_exception<std::invalid_argument>(
        [] { (void)RespSerializer(RespSerializer::Limits{1024, 10, 0}); },
        "zero nesting limit accepted");
}

void test_reuse_after_limit_failure() {
    const RespSerializer serializer(RespSerializer::Limits{12, 3, 2});

    expect_exception<std::length_error>(
        [&] { (void)serializer.serialize(RespValue::bulk_string(std::string(100, 'a'))); },
        "oversized payload was accepted");

    check_bytes(serializer, RespValue::integer(-12), ":-12\r\n",
                "serializer unusable after limit exception");

    check_bytes(serializer, RespValue::null_bulk_string(), "$-1\r\n",
                "serializer retained failed response data");
}

void test_default_output_boundary() {
    // With a seven-digit bulk length, RESP2 framing
    // adds exactly 12 bytes around the payload.
    constexpr std::size_t one_mib = 1024U * 1024U;

    std::string bytes(one_mib - 12U, 'X');

    const RespSerializer serializer;

    const std::string encoded = serializer.serialize(RespValue::bulk_string(bytes));

    const std::string prefix = "$" + std::to_string(bytes.size()) + "\r\n";

    check(prefix.size() == 10, "default bulk prefix length assumption changed");

    check(encoded.size() == one_mib, "exact default output boundary rejected");

    check(encoded.compare(0, prefix.size(), prefix) == 0, "large bulk header incorrect");

    check(encoded.compare(prefix.size(), bytes.size(), bytes) == 0, "large bulk payload corrupted");

    check(encoded.compare(encoded.size() - 2, 2, "\r\n") == 0, "large bulk trailing CRLF missing");

    bytes.push_back('Y');

    expect_exception<std::length_error>(
        [&] { (void)serializer.serialize(RespValue::bulk_string(bytes)); },
        "default output limit exceeded without rejection");

    check_bytes(serializer, RespValue::integer(1), ":1\r\n",
                "serializer unusable after large output rejection");
}

void test_tiny_output_budgets() {
    const RespSerializer three_bytes(RespSerializer::Limits{3, 1, 1});

    check_bytes(three_bytes, RespValue::simple_string(""), "+\r\n",
                "three-byte simple response rejected");

    check_bytes(three_bytes, RespValue::error(""), "-\r\n", "three-byte error response rejected");

    expect_exception<std::length_error>([&] { (void)three_bytes.serialize(RespValue::integer(0)); },
                                        "four-byte integer fit into three-byte budget");

    expect_exception<std::length_error>(
        [&] { (void)three_bytes.serialize(RespValue::null_bulk_string()); },
        "five-byte null bulk fit into three-byte budget");

    const RespSerializer four_bytes(RespSerializer::Limits{4, 1, 1});

    check_bytes(four_bytes, RespValue::integer(0), ":0\r\n",
                "exact four-byte integer boundary rejected");

    check_bytes(four_bytes, RespValue::error("E"), "-E\r\n",
                "exact four-byte error boundary rejected");
}

void test_nested_binary_and_depth_boundaries() {
    std::string binary("\0\r\n", 3);

    binary.push_back(static_cast<char>(0x80));
    binary.push_back('Z');

    const RespValue nested = RespValue::array({RespValue::array({RespValue::bulk_string(binary)})});

    std::string expected("*1\r\n*1\r\n$5\r\n");

    expected.append(binary);
    expected.append("\r\n");

    const RespSerializer exact(RespSerializer::Limits{expected.size(), 1, 2});

    check_bytes(exact, nested, expected, "nested binary payload at exact limits incorrect");

    const RespSerializer too_small(RespSerializer::Limits{expected.size() - 1, 1, 2});

    expect_exception<std::length_error>([&] { (void)too_small.serialize(nested); },
                                        "nested binary output exceeded byte budget");

    const RespSerializer too_shallow(RespSerializer::Limits{expected.size(), 1, 1});

    expect_exception<std::length_error>([&] { (void)too_shallow.serialize(nested); },
                                        "nested array exceeded configured depth");

    check_bytes(exact, RespValue::null_array(), "*-1\r\n",
                "serializer retained invalid nested state");
}

} // namespace

int main() {
    try {
        test_simple_strings_and_errors();
        test_signed_integers();
        test_binary_safe_bulk_strings();
        test_null_representations();
        test_nested_arrays();
        test_output_limit_exact_boundary();
        test_array_element_limits();
        test_nesting_depth_limits();
        test_cumulative_output_limit();
        test_invalid_configurations();
        test_reuse_after_limit_failure();
        test_default_output_boundary();
        test_tiny_output_budgets();
        test_nested_binary_and_depth_boundaries();

        std::cout << "RESP2 serializer tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RESP2 serializer test failed: " << error.what() << '\n';
        return 1;
    }
}
