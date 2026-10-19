#pragma once

#include "redisdb/protocol/RespValue.hpp"

#include <cstddef>
#include <string>

namespace redisdb::protocol {

// Encodes an owned RESP2 value into an owned byte string.
//
// No sockets, command execution or parser dependencies.
//
// Serialization is two-pass:
// 1. Validate limits and measure the exact response size.
// 2. Reserve the output and write the RESP2 framing.
//
// Exceeding a configured limit throws std::length_error.
// Invalid limit configuration throws std::invalid_argument.
class RespSerializer {
  public:
    struct Limits {
        std::size_t max_output_bytes = 1024 * 1024;
        std::size_t max_array_elements = 65536;
        std::size_t max_nesting_depth = 32;
    };

    RespSerializer();
    explicit RespSerializer(Limits limits);

    [[nodiscard]] std::string serialize(const RespValue& value) const;

  private:
    Limits limits_;
};

} // namespace redisdb::protocol
