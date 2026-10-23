#pragma once

#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace redisdb::storage {

// Stored bytes are always owned; never retain views into RESP/client buffers.
struct StringValue {
    std::string bytes;
    bool operator==(const StringValue&) const = default;
};

// A node list provides constant-time front/back operations and a noexcept
// move on the supported standard libraries. This preserves the storage
// variant's nonthrowing move behavior required by existing mutation paths.
// Per-element allocation is an explicit Phase 6B/14 performance tradeoff.
struct ListValue {
    std::list<std::string> elements;
    bool operator==(const ListValue&) const = default;
};

// Hash fields and values are independently owned binary-safe byte strings.
// Iteration order is unspecified.
struct HashValue {
    std::unordered_map<std::string, std::string> fields;
    bool operator==(const HashValue&) const = default;
};

// Set uniqueness is by exact byte-string equality; order is unspecified.
struct SetValue {
    std::unordered_set<std::string> members;
    bool operator==(const SetValue&) const = default;
};

// Storage and RESP2 protocol values deliberately remain separate models.
// Collection values are not yet reachable through Database's public API;
// command insertion and WRONGTYPE handling belong to later Phase 6 work.
using Value = std::variant<StringValue, ListValue, HashValue, SetValue>;

} // namespace redisdb::storage
