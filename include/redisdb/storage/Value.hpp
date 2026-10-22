#pragma once

#include <string>
#include <variant>

namespace redisdb::storage {

// An owned, binary-safe string value. New concrete alternatives
// join Value when collection types arrive in Phase 6.
struct StringValue {
    std::string bytes;
    bool operator==(const StringValue&) const = default;
};

// Storage values are intentionally separate from RESP2 wire values.
using Value = std::variant<StringValue>;

} // namespace redisdb::storage
