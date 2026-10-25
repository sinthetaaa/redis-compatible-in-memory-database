#pragma once

#include "redisdb/storage/Entry.hpp"

#include <cstddef>
#include <string_view>

namespace redisdb::storage::memory_accounting {

// Version-1 deterministic accounting *policy units*: NOT RSS or measured
// allocations. These uncalibrated coefficients are independent of platform,
// std::string capacity, allocator, hash bucket count and temporary clones.
// Memory limits enforce these same policy units without claiming an RSS bound.
inline constexpr std::size_t kKeyFixedBytes = 64;
inline constexpr std::size_t kListElementFixedBytes = 48;
inline constexpr std::size_t kHashFieldFixedBytes = 80;
inline constexpr std::size_t kSetMemberFixedBytes = 56;
inline constexpr std::size_t kExpiryFixedBytes = 48;

// Throw std::overflow_error rather than wrapping a size_t charge.
[[nodiscard]] std::size_t checked_add(std::size_t lhs, std::size_t rhs);
[[nodiscard]] std::size_t checked_multiply(std::size_t lhs, std::size_t rhs);

// Pure checked database-total transitions. Validate subtraction before it is
// performed, then add the replacement charge to the reduced total. This
// intentionally avoids a spurious overflow from total + new - previous.
// Throw std::logic_error for inconsistent existing charges, std::overflow_error
// for an unrepresentable new total. Neither function modifies live state.
[[nodiscard]] std::size_t replacement_total(std::size_t current_total, std::size_t old_entry_charge,
                                            std::size_t new_entry_charge);
[[nodiscard]] std::size_t removal_total(std::size_t current_total, std::size_t old_entry_charge);

// Pure, no mutation and no allocation for nonoverflowing inputs.
[[nodiscard]] std::size_t value_charge(const Value& value);
[[nodiscard]] std::size_t entry_charge(std::string_view key, const Entry& entry);

} // namespace redisdb::storage::memory_accounting
