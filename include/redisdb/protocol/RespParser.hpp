#pragma once

#include "redisdb/protocol/RespValue.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace redisdb::protocol {

enum class ParseStatus { Complete, NeedMore, Invalid };

struct ParseResult {
    ParseStatus status;
    std::optional<RespValue> value;
    std::size_t consumed_bytes = 0;
    std::string error;
};

// Stateless, single-frame RESP2 decoder.
//
// Complete:
//   - Owns exactly one decoded RespValue.
//   - Reports the number of bytes consumed.
//
// NeedMore:
//   - Input is a valid incomplete frame prefix.
//   - Consumes zero bytes and exposes no value.
//
// Invalid:
//   - Input is malformed or exceeds a configured limit.
//   - Consumes zero bytes and exposes no value.
//
// Buffer accumulation and multi-frame iteration
// are deliberately deferred to later phases.
class RespParser {
  public:
    struct Limits {
        std::size_t max_frame_bytes = 1024 * 1024;
        std::size_t max_bulk_bytes = 1024 * 1024;
        std::size_t max_array_elements = 65536;
        std::size_t max_nesting_depth = 32;
        std::size_t max_inline_bytes = 64 * 1024;
    };

    RespParser();
    explicit RespParser(Limits limits);

    [[nodiscard]] ParseResult parse(std::string_view input) const;

  private:
    [[nodiscard]] ParseResult parse_value(std::string_view input, std::size_t cursor,
                                          std::size_t depth) const;

    Limits limits_;
};

} // namespace redisdb::protocol
