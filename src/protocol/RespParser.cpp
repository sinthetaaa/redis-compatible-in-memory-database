#include "redisdb/protocol/RespParser.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace redisdb::protocol {
namespace {

ParseResult complete(RespValue value, std::size_t consumed) {
    return {ParseStatus::Complete, std::move(value), consumed, {}};
}

ParseResult need_more() { return {ParseStatus::NeedMore, std::nullopt, 0, {}}; }

ParseResult invalid(const char* message) {
    return {ParseStatus::Invalid, std::nullopt, 0, message};
}

enum class LineStatus { Complete, NeedMore, Invalid };

struct LineResult {
    LineStatus status;
    std::string_view text;
    std::size_t next = 0;
};

// Find a bounded RESP2 metadata line.
//
// CRLF is mandatory. Bare CR and LF are invalid.
// A trailing CR is incomplete if LF could still fit.
LineResult read_line(std::string_view input, std::size_t start, std::size_t frame_limit,
                     std::size_t inline_limit) {
    if (start >= frame_limit) {
        return {LineStatus::Invalid, {}, 0};
    }

    for (std::size_t pos = start; pos < input.size() && pos < frame_limit; ++pos) {
        const char ch = input[pos];

        if (ch == '\r') {
            if (pos - start > inline_limit || pos + 1 >= frame_limit) {
                return {LineStatus::Invalid, {}, 0};
            }

            if (pos + 1 == input.size()) {
                return {LineStatus::NeedMore, {}, 0};
            }

            if (input[pos + 1] != '\n') {
                return {LineStatus::Invalid, {}, 0};
            }

            return {LineStatus::Complete, input.substr(start, pos - start), pos + 2};
        }

        if (ch == '\n' || pos - start >= inline_limit) {
            return {LineStatus::Invalid, {}, 0};
        }
    }

    if (input.size() >= frame_limit || input.size() - start > inline_limit) {
        return {LineStatus::Invalid, {}, 0};
    }

    return {LineStatus::NeedMore, {}, 0};
}

// RESP integers permit an optional leading '+'
// or '-' sign. Length fields are more restrictive:
// only unsigned decimal or the null sentinel -1.
bool parse_signed(std::string_view text, bool permit_plus, std::int64_t& output) {
    if (text.empty()) {
        return false;
    }

    if (text.front() == '+') {
        if (!permit_plus) {
            return false;
        }

        text.remove_prefix(1);

        if (text.empty()) {
            return false;
        }
    }

    const auto first = text.data();
    const auto last = first + text.size();

    const auto result = std::from_chars(first, last, output, 10);

    return (result.ec == std::errc{} && result.ptr == last);
}

} // namespace

RespParser::RespParser() : RespParser(Limits{}) {}

RespParser::RespParser(Limits limits) : limits_(limits) {
    if (limits_.max_frame_bytes == 0 || limits_.max_bulk_bytes == 0 ||
        limits_.max_array_elements == 0 || limits_.max_nesting_depth == 0 ||
        limits_.max_inline_bytes == 0) {
        throw std::invalid_argument("RESP2 parser limits must be positive");
    }
}

ParseResult RespParser::parse(std::string_view input) const { return parse_value(input, 0, 0); }

ParseResult RespParser::parse_value(std::string_view input, std::size_t cursor,
                                    std::size_t depth) const {
    if (cursor >= limits_.max_frame_bytes) {
        return invalid("RESP2 frame exceeds byte limit");
    }

    if (cursor >= input.size()) {
        return need_more();
    }

    const char kind = input[cursor++];

    if (kind != '+' && kind != '-' && kind != ':' && kind != '$' && kind != '*') {
        return invalid("Unsupported RESP2 type prefix");
    }

    const auto line = read_line(input, cursor, limits_.max_frame_bytes, limits_.max_inline_bytes);

    if (line.status == LineStatus::NeedMore) {
        return need_more();
    }

    if (line.status == LineStatus::Invalid) {
        return invalid("Invalid RESP2 metadata line");
    }

    cursor = line.next;

    if (kind == '+') {
        return complete(RespValue::simple_string(std::string(line.text)), cursor);
    }

    if (kind == '-') {
        return complete(RespValue::error(std::string(line.text)), cursor);
    }

    std::int64_t number = 0;

    if (!parse_signed(line.text, kind == ':', number)) {
        return invalid("Invalid RESP2 integer or length");
    }

    if (kind == ':') {
        return complete(RespValue::integer(number), cursor);
    }

    if (number == -1) {
        return complete(kind == '$' ? RespValue::null_bulk_string() : RespValue::null_array(),
                        cursor);
    }

    if (number < 0) {
        return invalid("Invalid negative RESP2 length");
    }

    // Validation precedes conversion to size_t.
    const auto count = static_cast<std::uint64_t>(number);

    if (kind == '$') {
        if (count > limits_.max_bulk_bytes) {
            return invalid("RESP2 bulk length exceeds limit");
        }

        const std::size_t length = static_cast<std::size_t>(count);

        const std::size_t remaining_budget = limits_.max_frame_bytes - cursor;

        if (length > remaining_budget || remaining_budget - length < 2) {
            return invalid("RESP2 bulk frame exceeds byte limit");
        }

        const std::size_t available = input.size() - cursor;

        // A visible first terminator byte must already be CR,
        // even when the following LF has not arrived yet.
        if (available > length && input[cursor + length] != '\r') {
            return invalid("Invalid RESP2 bulk terminator");
        }

        if (available < length + 2) {
            return need_more();
        }

        if (input[cursor + length + 1] != '\n') {
            return invalid("Invalid RESP2 bulk terminator");
        }

        return complete(RespValue::bulk_string(std::string(input.substr(cursor, length))),
                        cursor + length + 2);
    }

    if (count > limits_.max_array_elements) {
        return invalid("RESP2 array element count exceeds limit");
    }

    if (depth >= limits_.max_nesting_depth) {
        return invalid("RESP2 nesting depth exceeds limit");
    }

    std::vector<RespValue> elements;

    // Never preallocate from an untrusted declared
    // count before its actual elements are decoded.
    for (std::uint64_t i = 0; i < count; ++i) {
        auto child = parse_value(input, cursor, depth + 1);

        if (child.status != ParseStatus::Complete) {
            return child;
        }

        cursor = child.consumed_bytes;

        elements.push_back(std::move(*child.value));
    }

    return complete(RespValue::array(std::move(elements)), cursor);
}

} // namespace redisdb::protocol
