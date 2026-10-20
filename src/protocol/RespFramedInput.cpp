#include "redisdb/protocol/RespFramedInput.hpp"

#include "redisdb/net/Connection.hpp"

#include <stdexcept>

namespace redisdb::protocol {

RespFramedInput::RespFramedInput()
    : RespFramedInput(RespParser::Limits{64 * 1024, 64 * 1024, 65536, 32, 64 * 1024}) {}

// The default 64 KiB frame budget matches the
// default Connection::Limits::max_input_bytes.
//
// A caller using a custom connection input limit
// must configure a compatible parser frame budget.
RespFramedInput::RespFramedInput(RespParser::Limits limits) : parser_(limits) {}

ParseResult RespFramedInput::peek(const net::Connection& connection) const {
    return parser_.parse(connection.input());
}

ParseResult RespFramedInput::poll(net::Connection& connection) const {
    ParseResult result = peek(connection);

    if (result.status != ParseStatus::Complete) {
        // NeedMore and Invalid never consume input.
        return result;
    }

    if (!result.value.has_value() || result.consumed_bytes == 0 ||
        result.consumed_bytes > connection.input().size()) {
        throw std::logic_error("RESP2 parser violated Complete contract");
    }

    // RespValue already owns its decoded content.
    // Consuming the wire bytes cannot invalidate it.
    connection.consume_input(result.consumed_bytes);

    return result;
}

} // namespace redisdb::protocol
