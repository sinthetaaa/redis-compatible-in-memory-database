#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace redisdb::protocol {

class RespValue;

struct SimpleString {
    std::string text;
    bool operator==(const SimpleString&) const = default;
};

struct Error {
    std::string message;
    bool operator==(const Error&) const = default;
};

struct Integer {
    std::int64_t value;
    bool operator==(const Integer&) const = default;
};

struct BulkString {
    std::string bytes;
    bool operator==(const BulkString&) const = default;
};

struct NullBulkString {
    bool operator==(const NullBulkString&) const = default;
};

struct Array {
    std::vector<RespValue> elements;
    bool operator==(const Array&) const = default;
};

struct NullArray {
    bool operator==(const NullArray&) const = default;
};

// Owned RESP2 syntax tree.
//
// No borrowed views, wire framing or socket dependencies.
// Framing and resource limits belong to later protocol phases.
class RespValue {
  public:
    using Storage =
        std::variant<SimpleString, Error, Integer, BulkString, NullBulkString, Array, NullArray>;

    static RespValue simple_string(std::string text);
    static RespValue error(std::string message);
    static RespValue integer(std::int64_t value);
    static RespValue bulk_string(std::string bytes);
    static RespValue null_bulk_string();
    static RespValue array(std::vector<RespValue> elements);
    static RespValue null_array();

    [[nodiscard]] const Storage& storage() const noexcept { return storage_; }

    [[nodiscard]] bool operator==(const RespValue&) const = default;

  private:
    explicit RespValue(Storage storage);

    Storage storage_;
};

} // namespace redisdb::protocol
