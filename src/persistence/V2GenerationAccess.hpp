#pragma once

#include "redisdb/persistence/V2Generation.hpp"

// Private typed bridge. RootState and descriptor operations remain in V2Root.cpp.
// No arbitrary-name, raw-descriptor, second-lock or publication capability.
namespace redisdb::persistence::v2::detail {
struct GenerationAccess final {
    static void borrow(GenerationCandidate&, RootLease&) noexcept;
    static bool healthy(const GenerationCandidate&) noexcept;
    static RootError error(const GenerationCandidate&) noexcept;
    static void search(GenerationCandidate&, GenerationId, LogicalSequence, std::uint32_t) noexcept;
    static bool check(GenerationCandidate&) noexcept;
    static RootError release(GenerationCandidate&) noexcept;
};
} // namespace redisdb::persistence::v2::detail
