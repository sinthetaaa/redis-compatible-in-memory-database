#pragma once

#include "redisdb/persistence/V2Staging.hpp"

// Internal typed bridge only. RootState stays defined in V2Root.cpp. No raw
// descriptor, arbitrary name, reopen, lock acquisition or publication interface.
namespace redisdb::persistence::v2::detail {
struct StagingAccess final {
    static void borrow(StagedFile&, RootLease&) noexcept;
    static bool healthy(const StagedFile&) noexcept;
    static RootError error(const StagedFile&) noexcept;
    static bool fail(StagedFile&, RootErrorCode, RootOperation) noexcept;
    static bool create(StagedFile&) noexcept;
    static bool check(StagedFile&) noexcept;
    static bool append(StagedFile&, ByteView) noexcept;
    static bool read(StagedFile&, std::uint64_t, std::span<std::byte>) noexcept;
    static bool sync(StagedFile&) noexcept;
    static bool unlink(StagedFile&) noexcept;
    static bool close(StagedFile&) noexcept;
};
} // namespace redisdb::persistence::v2::detail
