#pragma once

#include "redisdb/storage/Entry.hpp"

#include <cstddef>
#include <string_view>

namespace redisdb::storage::memory_accounting {

// Version-1 deterministic accounting *policy units*: NOT RSS or measured
// allocations. These uncalibrated coefficients are independent of platform,
// std::string capacity, allocator, hash bucket count and temporary clones.
// Revisit/calibrate before Phase 7 memory-limit enforcement.
inline constexpr std::size_t kKeyFixedBytes = 64;
inline constexpr std::size_t kListElementFixedBytes = 48;
inline constexpr std::size_t kHashFieldFixedBytes = 80;
inline constexpr std::size_t kSetMemberFixedBytes = 56;
inline constexpr std::size_t kExpiryFixedBytes = 48;

// Throw std::overflow_error rather than wrapping a size_t charge.
[[nodiscard]] std::size_t checked_add(std::size_t lhs, std::size_t rhs);
[[nodiscard]] std::size_t checked_multiply(std::size_t lhs, std::size_t rhs);

// Pure, no mutation and no allocation for nonoverflowing inputs.
[[nodiscard]] std::size_t value_charge(const Value& value);
[[nodiscard]] std::size_t entry_charge(std::string_view key, const Entry& entry);

} // namespace redisdb::storage::memory_accounting
