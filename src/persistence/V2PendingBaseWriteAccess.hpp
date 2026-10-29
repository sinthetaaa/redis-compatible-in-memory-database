#pragma once

#include "redisdb/persistence/V2PendingBaseWrite.hpp"

// Private typed ownership bridge. RootState stays in V2Root.cpp. No descriptor,
// name, mutable child, generic sink or publication capability escapes.
namespace redisdb::persistence::v2::detail {
struct PendingBaseWriteAccess final {
    static bool transfer(PendingBaseWriter&, PendingGeneration&) noexcept;
    static bool healthy(const PendingBaseWriter&) noexcept;
    static RootError error(const PendingBaseWriter&) noexcept;
    static bool check(PendingBaseWriter&) noexcept;
    static void fail(PendingBaseWriter&, RootErrorCode) noexcept;
    // Caller drains children/candidate first; capture final close error before
    // resetting the last root reference. No fabricated PendingGeneration owner.
    static RootError release(PendingBaseWriter&) noexcept;
};
} // namespace redisdb::persistence::v2::detail
