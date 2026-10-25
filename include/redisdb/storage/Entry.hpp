#pragma once

#include "redisdb/storage/Clock.hpp"
#include "redisdb/storage/Value.hpp"

#include <cstddef>
#include <list>
#include <optional>
#include <string>

namespace redisdb::storage {

// Owns its value and an optional absolute wall-clock expiration deadline.
// Phase 5B only records metadata. Visibility/removal of expired keys belongs
// to Phase 5C, and TTL command semantics belong to later subphases.
// A default-created entry has no deadline; historical Entry{Value{...}}
// aggregate initializers remain valid.
struct Entry {
    Value value;
    std::optional<WallTimePoint> expires_at{};
    // V1 logical charge, not allocator usage; refreshed on every physical mutation.
    std::size_t accounted_charge = 0;
    // Database-owned exact LRU index; valid only under AllKeysLru.
    std::list<std::string>::iterator lru_position{};
};

} // namespace redisdb::storage
