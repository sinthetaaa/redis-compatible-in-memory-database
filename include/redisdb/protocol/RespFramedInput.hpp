#pragma once

#include "redisdb/protocol/RespParser.hpp"

namespace redisdb::net {
class Connection;
}

namespace redisdb::protocol {

// Connects the stateless RESP2 parser to the
// existing per-connection input buffer.
//
// This class owns no receive buffer and performs
// no socket I/O.
//
// Exactly one frame is attempted per poll().
//
// Complete:
//   - Returns an owned decoded value.
//   - Consumes exactly the completed frame.
//
// NeedMore:
//   - Retains all buffered input.
//
// Invalid:
//   - Retains input for the caller's close policy.
//
// Automatically draining pipelined frames is deferred
// to Phase 2E.
class RespFramedInput final {
  public:
    RespFramedInput();

    explicit RespFramedInput(RespParser::Limits limits);

    [[nodiscard]] ParseResult poll(net::Connection& connection) const;

  private:
    RespParser parser_;
};

} // namespace redisdb::protocol
