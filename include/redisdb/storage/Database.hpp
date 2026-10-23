#pragma once

#include "redisdb/storage/Entry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace redisdb::storage {

// Heterogeneous lookup: no temporary string allocation for get/contains/erase.
struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};

// Strict ASCII signed 64-bit decimal parsing, independent of locale.
// Accepts optional minus sign; rejects plus, whitespace, leading zeroes,
// empty values, embedded bytes, and out-of-range magnitudes.
[[nodiscard]] std::optional<std::int64_t> parse_integer64(std::string_view text) noexcept;

enum class IntegerMutationError { None, InvalidInteger, Overflow };
struct IntegerMutationResult {
    IntegerMutationError error = IntegerMutationError::None;
    std::int64_t value = 0;
};

// Authoritative, process-local, binary-safe keyspace.
// The caller serializes access; this class is not independently synchronized.
// Phase 5B stores optional expiration metadata; enforcement and TTL commands
// are not yet implemented. No eviction or persistence exists.
class Database final {
  public:
    Database() = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    // Returns true if a new key was created, false if an existing key was replaced.
    // Stages a complete owned replacement before mutating an existing entry.
    [[nodiscard]] bool set(std::string key, std::string bytes);

    // Atomic multi-key replacement, with last-wins semantics for repeated keys.
    // Clone and swap provides strong exception safety; optimized delta
    // staging is deferred to a separate measured performance milestone.
    void set_many(const std::vector<std::pair<std::string, std::string>>& pairs);

    // Atomically append binary bytes to a string, creating a missing key.
    // The caller selects a storage limit; nullopt means the new value would
    // exceed the limit and guarantees that no mutation occurred.
    // The new value is fully staged before modifying an existing entry.
    [[nodiscard]] std::optional<std::size_t> append(std::string_view key, std::string_view suffix,
                                                    std::size_t max_value_bytes);

    // Returns byte length, not Unicode code-point count; missing keys are zero.
    [[nodiscard]] std::size_t string_length(std::string_view key) const;

    // Atomic signed 64-bit arithmetic. `subtract` avoids negating INT64_MIN.
    // Missing keys start at zero. Any error preserves the previous value or
    // absence. No arithmetic may overflow a signed C++ integer.
    [[nodiscard]] IntegerMutationResult adjust_integer(std::string_view key, std::int64_t operand,
                                                       bool subtract);

    // Metadata-only deadline operations (5B). These methods do NOT enforce
    // expiration or compare the deadline against the clock. A past deadline
    // remains visible until Phase 5C introduces lazy expiration.
    // No implicit key creation; false if a key is absent.
    [[nodiscard]] bool set_expiration(std::string_view key, WallTimePoint deadline);
    [[nodiscard]] std::optional<WallTimePoint> expiration(std::string_view key) const;
    // Returns true only when an existing key had metadata to remove.
    [[nodiscard]] bool clear_expiration(std::string_view key);

    // Returns an owned snapshot, independent of future mutations or rehashing.
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
    [[nodiscard]] bool exists(std::string_view key) const;
    [[nodiscard]] bool erase(std::string_view key);
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    void clear() noexcept { entries_.clear(); }

  private:
    std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>> entries_;
};

} // namespace redisdb::storage
