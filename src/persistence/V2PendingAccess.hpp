#pragma once

#include "redisdb/persistence/V2PendingGeneration.hpp"

// Narrow private bridge. RootState/descriptors stay in V2Root.cpp; no new
// arbitrary-name/FD, syscall, lock, content, selection or publication interface.
namespace redisdb::persistence::v2::detail {
struct PendingAccess final {
    static bool claim(PendingGeneration&, RootLease&) noexcept;
    static bool healthy(const PendingGeneration&) noexcept;
    static RootError error(const PendingGeneration&) noexcept;
    static bool fail(PendingGeneration&, RootErrorCode, RootOperation, RootRole) noexcept;
    // Drain children/candidate first. Clear only the owned slot; capture final
    // LOCK/root close failure BEFORE resetting the last shared root reference.
    static RootError release(PendingGeneration&) noexcept;
};
} // namespace redisdb::persistence::v2::detail
