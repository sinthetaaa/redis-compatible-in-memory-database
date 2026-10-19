#include "redisdb/protocol/RespSerializer.hpp"

#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>

namespace redisdb::protocol {
namespace {

using Limits = RespSerializer::Limits;

// Signed 64-bit integers and size_t decimal representations
// fit inside this fixed-size stack buffer.
constexpr std::size_t decimal_buffer_size = 32;

template <typename Number> std::size_t decimal_length(Number number) {
    char buffer[decimal_buffer_size]{};

    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);

    if (result.ec != std::errc{}) {
        throw std::logic_error("RESP2 decimal formatting failed");
    }

    return static_cast<std::size_t>(result.ptr - buffer);
}

template <typename Number> void append_decimal(std::string& output, Number number) {
    char buffer[decimal_buffer_size]{};

    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);

    if (result.ec != std::errc{}) {
        throw std::logic_error("RESP2 decimal formatting failed");
    }

    output.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

// total is always <= limit, so limit - total
// cannot underflow. This protects all size additions.
void checked_add(std::size_t& total, std::size_t amount, std::size_t limit) {
    if (amount > limit - total) {
        throw std::length_error("RESP2 serialized response exceeds output limit");
    }

    total += amount;
}

void measure_value(const RespValue& value, const Limits& limits, std::size_t depth,
                   std::size_t& total) {
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;

            if constexpr (std::is_same_v<T, SimpleString>) {
                // '+' + payload + CRLF
                checked_add(total, 3, limits.max_output_bytes);
                checked_add(total, item.text.size(), limits.max_output_bytes);
            } else if constexpr (std::is_same_v<T, Error>) {
                // '-' + payload + CRLF
                checked_add(total, 3, limits.max_output_bytes);
                checked_add(total, item.message.size(), limits.max_output_bytes);
            } else if constexpr (std::is_same_v<T, Integer>) {
                // ':' + decimal integer + CRLF
                checked_add(total, 3 + decimal_length(item.value), limits.max_output_bytes);
            } else if constexpr (std::is_same_v<T, BulkString>) {
                // '$' + decimal length + CRLF + bytes + CRLF
                checked_add(total, 5 + decimal_length(item.bytes.size()), limits.max_output_bytes);

                checked_add(total, item.bytes.size(), limits.max_output_bytes);
            } else if constexpr (std::is_same_v<T, NullBulkString>) {
                checked_add(total, 5, limits.max_output_bytes);
            } else if constexpr (std::is_same_v<T, Array>) {
                if (depth >= limits.max_nesting_depth) {
                    throw std::length_error("RESP2 array nesting limit exceeded");
                }

                if (item.elements.size() > limits.max_array_elements) {
                    throw std::length_error("RESP2 array element limit exceeded");
                }

                // '*' + decimal element count + CRLF
                checked_add(total, 3 + decimal_length(item.elements.size()),
                            limits.max_output_bytes);

                for (const auto& child : item.elements) {
                    measure_value(child, limits, depth + 1, total);
                }
            } else if constexpr (std::is_same_v<T, NullArray>) {
                checked_add(total, 5, limits.max_output_bytes);
            }
        },
        value.storage());
}

void append_value(std::string& output, const RespValue& value) {
    // Every value and limit was checked in the
    // measurement pass before this function runs.
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;

            if constexpr (std::is_same_v<T, SimpleString>) {
                output.push_back('+');
                output.append(item.text);
                output.append("\r\n");
            } else if constexpr (std::is_same_v<T, Error>) {
                output.push_back('-');
                output.append(item.message);
                output.append("\r\n");
            } else if constexpr (std::is_same_v<T, Integer>) {
                output.push_back(':');
                append_decimal(output, item.value);
                output.append("\r\n");
            } else if constexpr (std::is_same_v<T, BulkString>) {
                output.push_back('$');
                append_decimal(output, item.bytes.size());
                output.append("\r\n");
                output.append(item.bytes);
                output.append("\r\n");
            } else if constexpr (std::is_same_v<T, NullBulkString>) {
                output.append("$-1\r\n");
            } else if constexpr (std::is_same_v<T, Array>) {
                output.push_back('*');
                append_decimal(output, item.elements.size());
                output.append("\r\n");

                for (const auto& child : item.elements) {
                    append_value(output, child);
                }
            } else if constexpr (std::is_same_v<T, NullArray>) {
                output.append("*-1\r\n");
            }
        },
        value.storage());
}

} // namespace

RespSerializer::RespSerializer() : RespSerializer(Limits{}) {}

RespSerializer::RespSerializer(Limits limits) : limits_(limits) {
    if (limits_.max_output_bytes == 0 || limits_.max_array_elements == 0 ||
        limits_.max_nesting_depth == 0) {
        throw std::invalid_argument("RESP2 serializer limits must be positive");
    }
}

std::string RespSerializer::serialize(const RespValue& value) const {
    std::size_t required_bytes = 0;

    measure_value(value, limits_, 0, required_bytes);

    std::string output;
    output.reserve(required_bytes);

    append_value(output, value);

    if (output.size() != required_bytes) {
        throw std::logic_error("RESP2 serialized size does not match preflight");
    }

    return output;
}

} // namespace redisdb::protocol
