#include "redisdb/protocol/RespValue.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace redisdb::protocol {
namespace {

void validate_inline_text(std::string_view value) {
    if (value.find_first_of("\r\n") != std::string_view::npos) {
        throw std::invalid_argument("RESP2 simple strings and errors cannot contain CR or LF");
    }
}

} // namespace

RespValue::RespValue(Storage storage) : storage_(std::move(storage)) {}

RespValue RespValue::simple_string(std::string text) {
    validate_inline_text(text);
    return RespValue(SimpleString{std::move(text)});
}

RespValue RespValue::error(std::string message) {
    validate_inline_text(message);
    return RespValue(Error{std::move(message)});
}

RespValue RespValue::integer(std::int64_t value) { return RespValue(Integer{value}); }

RespValue RespValue::bulk_string(std::string bytes) {
    return RespValue(BulkString{std::move(bytes)});
}

RespValue RespValue::null_bulk_string() { return RespValue(NullBulkString{}); }

RespValue RespValue::array(std::vector<RespValue> elements) {
    return RespValue(Array{std::move(elements)});
}

RespValue RespValue::null_array() { return RespValue(NullArray{}); }

} // namespace redisdb::protocol
