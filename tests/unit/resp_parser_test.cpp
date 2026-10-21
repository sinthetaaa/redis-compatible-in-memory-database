#include "redisdb/protocol/RespParser.hpp"
#include "redisdb/protocol/RespSerializer.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using redisdb::protocol::ParseStatus;
using redisdb::protocol::RespParser;
using redisdb::protocol::RespSerializer;
using redisdb::protocol::RespValue;

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_complete(const RespParser& parser, std::string_view frame, RespValue expected) {
    const auto result = parser.parse(frame);

    check(result.status == ParseStatus::Complete, "expected Complete");

    check(result.value.has_value(), "Complete must contain a value");

    check(*result.value == expected, "decoded RESP2 value mismatch");

    check(result.consumed_bytes == frame.size(), "incorrect consumed byte count");

    check(result.error.empty(), "Complete should not carry an error");
}

void expect_need_more(const RespParser& parser, std::string_view input) {
    const auto result = parser.parse(input);

    check(result.status == ParseStatus::NeedMore, "expected NeedMore");

    check(!result.value.has_value(), "NeedMore exposed a partial value");

    check(result.consumed_bytes == 0, "NeedMore consumed input");

    check(result.error.empty(), "NeedMore carried an error");
}

void expect_invalid(const RespParser& parser, std::string_view input) {
    const auto result = parser.parse(input);

    check(result.status == ParseStatus::Invalid, "expected Invalid");

    check(!result.value.has_value(), "Invalid exposed a value");

    check(result.consumed_bytes == 0, "Invalid consumed input");

    check(!result.error.empty(), "Invalid must provide a reason");
}

void test_scalar_values() {
    const RespParser parser;

    expect_complete(parser, "+OK\r\n", RespValue::simple_string("OK"));

    expect_complete(parser, "+\r\n", RespValue::simple_string(""));

    expect_complete(parser, "-ERR invalid\r\n", RespValue::error("ERR invalid"));

    expect_complete(parser, ":0\r\n", RespValue::integer(0));

    expect_complete(parser, ":+12\r\n", RespValue::integer(12));

    expect_complete(parser, ":-42\r\n", RespValue::integer(-42));

    expect_complete(parser, ":-9223372036854775808\r\n",
                    RespValue::integer(std::numeric_limits<std::int64_t>::min()));

    expect_complete(parser, ":9223372036854775807\r\n",
                    RespValue::integer(std::numeric_limits<std::int64_t>::max()));
}

void test_bulk_and_null_values() {
    const RespParser parser;

    expect_complete(parser, "$5\r\nhello\r\n", RespValue::bulk_string("hello"));

    expect_complete(parser, "$0\r\n\r\n", RespValue::bulk_string(""));

    expect_complete(parser, "$-1\r\n", RespValue::null_bulk_string());

    expect_complete(parser, "*-1\r\n", RespValue::null_array());

    expect_complete(parser, "*0\r\n", RespValue::array({}));

    std::string binary("\0\r\n", 3);
    binary.push_back(static_cast<char>(0xff));

    std::string frame("$4\r\n");
    frame.append(binary);
    frame.append("\r\n");

    expect_complete(parser, frame, RespValue::bulk_string(binary));
}

void test_recursive_arrays() {
    const RespParser parser;

    expect_complete(parser, "*2\r\n+OK\r\n:2\r\n",
                    RespValue::array({RespValue::simple_string("OK"), RespValue::integer(2)}));

    expect_complete(parser, "*1\r\n*2\r\n$-1\r\n*-1\r\n",
                    RespValue::array({RespValue::array(
                        {RespValue::null_bulk_string(), RespValue::null_array()})}));

    expect_complete(
        parser, "*1\r\n*1\r\n*1\r\n:7\r\n",
        RespValue::array({RespValue::array({RespValue::array({RespValue::integer(7)})})}));
}

void test_incomplete_frame_prefixes() {
    const RespParser parser;

    const std::vector<std::string> frames = {
        "+OK\r\n", ":1234\r\n", std::string("$3\r\na\0b\r\n", 9), "*2\r\n+OK\r\n$5\r\nhello\r\n",
        "*1\r\n*2\r\n$-1\r\n*-1\r\n"};

    for (const auto& frame : frames) {
        for (std::size_t length = 0; length < frame.size(); ++length) {
            expect_need_more(parser, std::string_view(frame.data(), length));
        }

        check(parser.parse(frame).status == ParseStatus::Complete, "complete frame did not finish");
    }

    expect_need_more(parser, "+abc\r");

    expect_need_more(parser, "*2\r\n+OK\r\n");

    expect_need_more(parser, "$5\r\nhel");
}

void test_single_frame_consumption() {
    const RespParser parser;

    const auto result = parser.parse("+OK\r\n:7\r\n");

    check(result.status == ParseStatus::Complete, "first frame not decoded");

    check(result.consumed_bytes == 5, "parser consumed trailing frame");

    check(result.value.has_value(), "first frame value missing");

    check(*result.value == RespValue::simple_string("OK"), "first frame value incorrect");
}

void test_invalid_frames() {
    const RespParser parser;

    const std::vector<std::string> inputs = {"?OK\r\n",
                                             "+abc\n",
                                             "+a\rb\n",
                                             ":abc\r\n",
                                             ":+\r\n",
                                             ":9223372036854775808\r\n",
                                             ":-9223372036854775809\r\n",
                                             "$-2\r\n",
                                             "$+3\r\nabc\r\n",
                                             "$3\r\nabc\n\n",
                                             "*-3\r\n",
                                             "*abc\r\n",
                                             "*-9223372036854775808\r\n"};

    for (const auto& input : inputs) {
        expect_invalid(parser, input);
    }
}

void test_resource_limits() {
    expect_invalid(RespParser(RespParser::Limits{6, 100, 10, 4, 10}), "+abcd\r\n");

    expect_complete(RespParser(RespParser::Limits{7, 100, 10, 4, 10}), "+abcd\r\n",
                    RespValue::simple_string("abcd"));

    expect_invalid(RespParser(RespParser::Limits{10, 2, 10, 4, 10}), "$3\r\nabc\r\n");

    expect_invalid(RespParser(RespParser::Limits{100, 20, 1, 4, 10}), "*2\r\n");

    expect_invalid(RespParser(RespParser::Limits{100, 20, 5, 1, 10}), "*1\r\n*1\r\n:1\r\n");

    expect_invalid(RespParser(RespParser::Limits{100, 20, 5, 2, 2}), "+abc\r\n");

    expect_complete(RespParser(RespParser::Limits{100, 20, 5, 2, 3}), "+abc\r\n",
                    RespValue::simple_string("abc"));

    expect_invalid(RespParser(), "$1000000000\r\n");
}

void test_invalid_configuration() {
    bool rejected = false;

    try {
        (void)RespParser(RespParser::Limits{0, 100, 10, 4, 10});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    check(rejected, "zero frame limit accepted");
}

void test_parser_reuse() {
    const RespParser parser;

    expect_invalid(parser, ":not-a-number\r\n");

    expect_complete(parser, ":42\r\n", RespValue::integer(42));

    expect_need_more(parser, "$3\r\nab");

    expect_complete(parser, "$3\r\nabc\r\n", RespValue::bulk_string("abc"));
}

void test_serializer_roundtrips() {
    const RespParser parser;
    const RespSerializer serializer;

    std::string binary("\0A\r\n", 4);
    binary.push_back(static_cast<char>(0xff));

    const std::vector<RespValue> values = {
        RespValue::simple_string("OK"),
        RespValue::error("ERR invalid"),
        RespValue::integer(0),
        RespValue::integer(std::numeric_limits<std::int64_t>::min()),
        RespValue::integer(std::numeric_limits<std::int64_t>::max()),
        RespValue::bulk_string(""),
        RespValue::bulk_string(binary),
        RespValue::null_bulk_string(),
        RespValue::null_array(),
        RespValue::array({}),
        RespValue::array({RespValue::integer(-1), RespValue::bulk_string(binary),
                          RespValue::array({RespValue::null_bulk_string(), RespValue::null_array(),
                                            RespValue::simple_string("nested")})})};

    for (const auto& value : values) {
        const std::string frame = serializer.serialize(value);

        expect_complete(parser, frame, value);

        // Every proper prefix of a valid complete
        // single frame must report NeedMore.
        for (std::size_t n = 0; n < frame.size(); ++n) {
            expect_need_more(parser, std::string_view(frame.data(), n));
        }
    }
}

void test_exact_frame_boundaries() {
    const RespParser exact(RespParser::Limits{9, 3, 3, 3, 8});

    expect_complete(exact, "$3\r\nabc\r\n", RespValue::bulk_string("abc"));

    expect_need_more(exact, "$3\r\nab");

    const RespParser one_byte_short(RespParser::Limits{8, 3, 3, 3, 8});

    expect_invalid(one_byte_short, "$3\r\nabc\r\n");

    const RespParser bulk_too_small(RespParser::Limits{9, 2, 3, 3, 8});

    expect_invalid(bulk_too_small, "$3\r\nabc\r\n");

    const RespParser exact_inline(RespParser::Limits{7, 10, 3, 3, 4});

    expect_complete(exact_inline, "+abcd\r\n", RespValue::simple_string("abcd"));

    expect_invalid(exact_inline, "+abcde\r\n");

    expect_invalid(RespParser(RespParser::Limits{6, 10, 3, 3, 4}), "+abcd\r\n");
}

void test_partial_and_invalid_arrays() {
    const RespParser parser;

    expect_need_more(parser, "*2\r\n:1\r\n");

    expect_need_more(parser, "*2\r\n$3\r\nabc\r\n");

    expect_invalid(parser, "*1\r\n?invalid\r\n");

    expect_invalid(parser, "*1\r\n$3\r\nabc\n\n");

    expect_invalid(parser, "*65537\r\n");

    // The declared count is permitted, but the
    // actual array members have not arrived.
    expect_need_more(parser, "*65536\r\n");
}

void test_nesting_depth_exact_boundary() {
    const RespParser parser;

    std::string valid;

    for (int i = 0; i < 32; ++i) {
        valid.append("*1\r\n");
    }

    valid.append(":7\r\n");

    const auto result = parser.parse(valid);

    check(result.status == ParseStatus::Complete, "32-level array rejected");

    check(result.consumed_bytes == valid.size(), "32-level array consumed incorrect bytes");

    std::string too_deep("*1\r\n");
    too_deep.append(valid);

    expect_invalid(parser, too_deep);
}

void test_binary_frame_with_trailing_bytes() {
    const RespParser parser;

    std::string binary("\0\r\n", 3);
    binary.push_back(static_cast<char>(0x80));

    std::string first("$4\r\n");
    first.append(binary);
    first.append("\r\n");

    const std::string second = ":99\r\n";
    const std::string input = first + second;

    const auto result = parser.parse(input);

    check(result.status == ParseStatus::Complete, "first binary frame not complete");

    check(result.consumed_bytes == first.size(), "parser consumed beyond first binary frame");

    check(result.value.has_value(), "binary result missing");

    check(*result.value == RespValue::bulk_string(binary), "binary result corrupted");

    const std::string_view remaining(input.data() + result.consumed_bytes,
                                     input.size() - result.consumed_bytes);

    expect_complete(parser, remaining, RespValue::integer(99));
}

void test_early_bulk_terminator_rejection() {
    const RespParser parser;

    // Once the declared payload is complete, a known non-CR
    // terminator byte is malformed even if the LF is absent.
    const std::vector<std::string> invalid_prefixes = {"$3\r\nabcX", "$0\r\nX"};

    for (const std::string& prefix : invalid_prefixes) {
        const auto parsed = parser.parse(prefix);

        check(parsed.status == ParseStatus::Invalid,
              "Phase 2F: malformed first bulk terminator byte must be Invalid");
        check(!parsed.value.has_value(), "Invalid exposed a bulk value");
        check(parsed.consumed_bytes == 0, "Invalid consumed bulk input");
        check(!parsed.error.empty(), "Invalid omitted its error explanation");
    }

    expect_need_more(parser, "$3\r\nabc\r");
    expect_complete(parser, "$3\r\nabc\r\n", RespValue::bulk_string("abc"));
    expect_invalid(parser, "$3\r\nabc\rX");
}

void test_bulk_boundary_matrix_2f() {
    const RespParser parser;

    // A missing payload byte or a possible CRLF suffix is incomplete.
    expect_need_more(parser, "$3\r\nab");
    expect_need_more(parser, "$3\r\nabc");
    expect_need_more(parser, "$3\r\nabc\r");
    expect_complete(parser, "$3\r\nabc\r\n", RespValue::bulk_string("abc"));

    // A visible wrong terminator byte is already invalid, not NeedMore.
    expect_invalid(parser, "$3\r\nabcX");
    expect_invalid(parser, "$3\r\nabcX\n");
    expect_invalid(parser, "$3\r\nabc\rX");

    expect_need_more(parser, "$0\r\n");
    expect_need_more(parser, "$0\r\n\r");
    expect_complete(parser, "$0\r\n\r\n", RespValue::bulk_string(""));
    expect_invalid(parser, "$0\r\nX");
    expect_invalid(parser, "$0\r\n\rX");

    // The CRLF terminator is included in the total frame-byte budget.
    const RespParser exact(RespParser::Limits{9, 3, 3, 3, 8});
    expect_need_more(exact, "$3\r\nabc\r");
    expect_complete(exact, "$3\r\nabc\r\n", RespValue::bulk_string("abc"));
    const RespParser too_small(RespParser::Limits{8, 3, 3, 3, 8});
    expect_invalid(too_small, "$3\r\nabc");
    expect_invalid(too_small, "$3\r\nabc\r");

    // Embedded binary CRLF/NUL belong to the payload, never its terminator.
    const std::string binary("\0\r\n", 3);
    const std::string first = std::string("$3\r\n") + binary + "\r\n";
    expect_complete(parser, first, RespValue::bulk_string(binary));
    const auto first_result = parser.parse(first + ":7\r\n");
    check(first_result.status == ParseStatus::Complete, "binary pipeline first frame incomplete");
    check(first_result.consumed_bytes == first.size(), "binary pipeline consumed trailing frame");
    expect_complete(parser, ":7\r\n", RespValue::integer(7));

    // Child frame failure never exposes a partial enclosing array.
    expect_need_more(parser, "*2\r\n:7\r\n$3\r\nabc\r");
    expect_invalid(parser, "*2\r\n:7\r\n$3\r\nabcX");
    expect_complete(parser, "*2\r\n:7\r\n$3\r\nabc\r\n",
                    RespValue::array({RespValue::integer(7), RespValue::bulk_string("abc")}));
}

void test_canonical_length_spellings_2f() {
    const RespParser parser;

    // RESP integer values may carry a leading '+' sign; this test
    // restricts only bulk/array length fields, not integer values.
    expect_complete(parser, ":+12\r\n", RespValue::integer(12));

    // Canonical nonnegative lengths and exact null sentinels remain valid.
    expect_complete(parser, "$-1\r\n", RespValue::null_bulk_string());
    expect_complete(parser, "*-1\r\n", RespValue::null_array());
    expect_complete(parser, "$0\r\n\r\n", RespValue::bulk_string(""));
    expect_complete(parser, "*0\r\n", RespValue::array({}));
    expect_complete(parser, "$1\r\na\r\n", RespValue::bulk_string("a"));
    expect_complete(parser, "*1\r\n:7\r\n", RespValue::array({RespValue::integer(7)}));

    // Noncanonical length spellings must not be silently interpreted
    // as -1, zero, or a positive length. They must fail atomically.
    const std::vector<std::string_view> invalid_lengths = {
        "$-01\r\n",    "*-01\r\n", "$-0\r\n\r\n",  "*-0\r\n",
        "$00\r\n\r\n", "*00\r\n",  "$01\r\na\r\n", "*01\r\n:7\r\n"};

    for (std::string_view input : invalid_lengths) {
        const auto result = parser.parse(input);
        check(result.status == ParseStatus::Invalid,
              "Phase 2F: noncanonical RESP2 length spelling must be Invalid");
        check(!result.value.has_value(), "Invalid numeric length exposed a parsed value");
        check(result.consumed_bytes == 0, "Invalid numeric length consumed input");
        check(!result.error.empty(), "Invalid numeric length had no error explanation");
    }
}

} // namespace

int main() {
    try {
        test_scalar_values();
        test_bulk_and_null_values();
        test_recursive_arrays();
        test_incomplete_frame_prefixes();
        test_single_frame_consumption();
        test_invalid_frames();
        test_resource_limits();
        test_invalid_configuration();
        test_parser_reuse();
        test_serializer_roundtrips();
        test_exact_frame_boundaries();
        test_partial_and_invalid_arrays();
        test_nesting_depth_exact_boundary();
        test_binary_frame_with_trailing_bytes();
        test_early_bulk_terminator_rejection();
        test_bulk_boundary_matrix_2f();
        test_canonical_length_spellings_2f();

        std::cout << "RESP2 parser tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RESP2 parser test failed: " << error.what() << '\n';

        return 1;
    }
}
