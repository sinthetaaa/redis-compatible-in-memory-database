#include "RespParserFuzzChecks.hpp"

#include "redisdb/protocol/RespParser.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace redisdb::fuzz {
namespace {

using protocol::ParseResult;
using protocol::ParseStatus;
using protocol::RespParser;

[[noreturn]] void invariant_failed(const char* message) {
    std::fprintf(stderr, "RESP2 fuzz invariant violated: %s\n", message);
    std::abort();
}

void require(bool condition, const char* message) {
    if (!condition) {
        invariant_failed(message);
    }
}

const RespParser& bounded_parser() {
    static const RespParser parser{RespParser::Limits{
        4096, // maximum complete frame bytes
        2048, // maximum bulk payload bytes
        32,   // maximum elements in one array
        8,    // maximum array nesting depth
        256   // maximum metadata bytes per line
    }};
    return parser;
}

void verify_noncomplete(const ParseResult& result) {
    require(!result.value.has_value(), "non-Complete result exposes an owned value");
    require(result.consumed_bytes == 0, "non-Complete result consumes bytes");
    if (result.status == ParseStatus::NeedMore) {
        require(result.error.empty(), "NeedMore unexpectedly carries an error");
    } else if (result.status == ParseStatus::Invalid) {
        require(!result.error.empty(), "Invalid lacks an error reason");
    } else {
        invariant_failed("unknown parser status");
    }
}

} // namespace

void fuzz_one(std::string_view input) {
    if (input.size() > kMaxFuzzInputBytes) {
        return; // The foundation intentionally bounds each invocation.
    }

    const RespParser& parser = bounded_parser();
    const ParseResult result = parser.parse(input);

    if (result.status != ParseStatus::Complete) {
        verify_noncomplete(result);
        return;
    }

    require(result.value.has_value(), "Complete omitted its owned RespValue");
    require(result.error.empty(), "Complete unexpectedly carries an error");
    require(result.consumed_bytes > 0, "Complete consumes zero bytes");
    require(result.consumed_bytes <= input.size(), "Complete consumes beyond input");
    require(result.consumed_bytes <= kMaxFuzzInputBytes, "Complete exceeds frame limit");

    // This is deliberately a single-frame parser: arbitrary suffixes may
    // remain after the first decoded frame.
    const std::string_view first_frame = input.substr(0, result.consumed_bytes);
    const ParseResult replay = parser.parse(first_frame);
    require(replay.status == ParseStatus::Complete, "consumed frame does not replay");
    require(replay.value.has_value(), "replayed Complete lacks a value");
    require(replay.error.empty(), "replayed Complete carries an error");
    require(replay.consumed_bytes == first_frame.size(), "replay consumes wrong length");
    require(*replay.value == *result.value, "replayed owned value differs");

    // Sample at most eight proper prefixes, including zero and the
    // byte immediately before completion. Avoid quadratic prefix work.
    std::size_t last_checked = kMaxFuzzInputBytes + 1;
    for (std::size_t sample = 0; sample < 8; ++sample) {
        const std::size_t length = ((result.consumed_bytes - 1) * sample) / 7;
        if (length == last_checked) {
            continue;
        }
        last_checked = length;
        const ParseResult prefix = parser.parse(first_frame.substr(0, length));
        require(prefix.status == ParseStatus::NeedMore,
                "proper prefix of a completed frame is not NeedMore");
        verify_noncomplete(prefix);
    }
}

} // namespace redisdb::fuzz
