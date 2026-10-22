#pragma once

#include "redisdb/storage/Value.hpp"

namespace redisdb::storage {

// Owns its value. Expiration and accounting metadata are
// intentionally deferred to their corresponding phases.
struct Entry {
    Value value;
};

} // namespace redisdb::storage
