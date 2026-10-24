#pragma once

#include "redisdb/storage/Entry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
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

// Missing, string and collection identity after lazy expiration.
enum class StoredType { None, String, List, Hash, Set };

// Database callers can distinguish wrong types from missing keys or limits.
class WrongTypeError final : public std::logic_error {
  public:
    WrongTypeError()
        : std::logic_error("WRONGTYPE Operation against a key holding the wrong kind of value") {}
};

enum class IntegerMutationError { None, InvalidInteger, Overflow };
struct IntegerMutationResult {
    IntegerMutationError error = IntegerMutationError::None;
    std::int64_t value = 0;
};

// One atomic relative deadline operation, sampled against one wall-clock instant.
// The basic 5D command subset has no NX/XX/GT/LT option flags.
enum class ExpireResult { Missing, Set, Deleted, OutOfRange };

// SET with an expiration either atomically replaces value+deadline or does
// nothing. An invalid positive deadline must not modify the old key.
enum class SetWithExpiryResult { Set, OutOfRange };

// Authoritative, process-local, binary-safe keyspace.
// The caller serializes access; this class is not independently synchronized.
// Phase 5C enforces deadlines on demand, using an injectable wall clock.
// Logically const reads can remove expired entries; callers serialize access.
// TTL/PTTL/PERSIST are introduced in 5E; Phase 5G adds bounded active
// expiry. Eviction and durability/persistence to disk are not implemented.
class Database final {
  public:
    // The default database uses a process-lifetime production SystemClock.
    // An injected clock must outlive this Database (borrowed const reference).
    Database() noexcept;
    explicit Database(const Clock& clock) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    // Returns true if a new key was created, false if an existing key was replaced.
    // Stages a complete owned replacement before mutating an existing entry.
    [[nodiscard]] bool set(std::string key, std::string bytes);

    // Validate the full positive relative EX/PX deadline before mutation.
    // Stage value and deadline together; no intermediate persistent value.
    // This method does not impose command-layer reply-size limits.
    [[nodiscard]] SetWithExpiryResult set_with_expiration(std::string key, std::string bytes,
                                                          std::int64_t amount, bool milliseconds);

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

    // Expiration metadata operations never create keys. An already-expired
    // key is purged and treated as absent before metadata is read or changed.
    // Attaching a past deadline makes the key logically absent on next access.
    [[nodiscard]] bool set_expiration(std::string_view key, WallTimePoint deadline);
    [[nodiscard]] std::optional<WallTimePoint> expiration(std::string_view key) const;
    // Returns true only when an existing key had metadata to remove.
    [[nodiscard]] bool clear_expiration(std::string_view key);

    // Relative expiry in whole seconds or milliseconds. Non-positive values
    // delete an existing key; values too large for system_clock are rejected
    // without changing a live key's value/deadline.
    [[nodiscard]] ExpireResult expire_after(std::string_view key, std::int64_t amount,
                                            bool milliseconds);

    // Redis-compatible remaining lifetime. -2: absent/expired; -1: persistent.
    // Seconds are rounded to nearest integer; milliseconds truncate.
    // One clock sample, with overflow-safe signed-epoch subtraction.
    [[nodiscard]] std::int64_t remaining_ttl(std::string_view key, bool milliseconds) const;

    struct SweepResult {
        std::size_t inspected = 0;
        std::size_t deleted = 0;
    };
    // Deterministic, bounded maintenance turn: inspect at most `budget`
    // earliest deadlines. No full-table scan; unchanged live keys are safe.
    // The caller serializes this with all other database operations.
    [[nodiscard]] SweepResult sweep_expired(std::size_t budget);

    // Phase 6B.1: collection type identity, with live-key/lazy-TTL semantics.
    [[nodiscard]] StoredType type_of(std::string_view key) const;

    // Owned list primitives. All inserts stage the complete new list first;
    // wrong types throw WrongTypeError without mutation. Empty push input is
    // invalid. Existing TTL survives mutations; removing the last item deletes
    // the key and its expiration index record.
    [[nodiscard]] std::size_t list_push(std::string key, const std::vector<std::string>& values,
                                        bool left);
    [[nodiscard]] std::size_t list_length(std::string_view key) const;
    [[nodiscard]] std::optional<std::string> list_pop(std::string_view key, bool left);
    // Inclusive indexes; negative indexes are relative to the end. Absence or
    // an empty range returns an empty vector. A nullopt signals that the
    // encoded RESP2 array would exceed max_reply_bytes (no mutation).
    [[nodiscard]] std::optional<std::vector<std::string>>
    list_range(std::string_view key, std::int64_t start, std::int64_t stop,
               std::size_t max_reply_bytes) const;

    // Phase 6C.1: binary-safe hash storage. Like list writes, a complete
    // replacement is staged before mutation; wrong-type operations never
    // mutate the live entry. Hash field ordering is deliberately unspecified.
    [[nodiscard]] std::size_t
    hash_set(std::string key, const std::vector<std::pair<std::string, std::string>>& fields);
    [[nodiscard]] std::optional<std::string> hash_get(std::string_view key,
                                                      std::string_view field) const;
    [[nodiscard]] bool hash_exists(std::string_view key, std::string_view field) const;
    [[nodiscard]] std::size_t hash_length(std::string_view key) const;
    [[nodiscard]] std::size_t hash_delete(std::string_view key,
                                          const std::vector<std::string>& fields);
    // RESP2 HGETALL-shaped field/value array. Missing keys yield an empty
    // vector, while nullopt signals an exceeded reply budget. Read-only.
    [[nodiscard]] std::optional<std::vector<std::pair<std::string, std::string>>>
    hash_items(std::string_view key, std::size_t max_reply_bytes) const;

    // Phase 6D.1: binary-safe set storage, no wire routing in this milestone.
    // Member equality is byte-exact; iteration order is unspecified. All set
    // additions/removals stage a full replacement to preserve atomicity.
    // TTL survives a nonempty mutation; removing the final member removes the
    // key and associated deadline index entry. Wrong types never mutate.
    [[nodiscard]] std::size_t set_add(std::string key, const std::vector<std::string>& members);
    [[nodiscard]] std::size_t set_remove(std::string_view key,
                                         const std::vector<std::string>& members);
    [[nodiscard]] bool set_contains(std::string_view key, std::string_view member) const;
    [[nodiscard]] std::size_t set_cardinality(std::string_view key) const;
    // Owned unordered snapshot, bounded by the exact RESP2 SMEMBERS byte size.
    // Missing/expired returns an empty vector; nullopt indicates over budget.
    [[nodiscard]] std::optional<std::vector<std::string>>
    set_members(std::string_view key, std::size_t max_reply_bytes) const;

    // Returns an owned string snapshot. A non-string key throws WrongTypeError;
    // an absent or expired key returns nullopt, never an implicit conversion.
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
    [[nodiscard]] bool exists(std::string_view key) const;
    [[nodiscard]] bool erase(std::string_view key);
    // Physical entry count. With lazy-only expiration, untouched expired
    // entries can remain until accessed (5G adds active sweeping).
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    // O(1) deterministic logical charged bytes. Physically present stale keys count.
    [[nodiscard]] std::size_t accounted_bytes() const noexcept { return accounted_bytes_; }
    void clear() noexcept {
        entries_.clear();
        expiry_index_.clear();
        accounted_bytes_ = 0;
    }

  private:
    // Treat deadline <= current wall time as expired. This is logically
    // const but removes stale physical entries; no background sweep occurs.
    [[nodiscard]] bool expire_if_needed(std::string_view key) const;
    const Clock& clock_;
    using EntryMap = std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>>;
    void insert_new(std::string key, Entry staged);
    void replace_value(EntryMap::iterator found, Value staged);
    void erase_entry(EntryMap::iterator found) const;
    mutable EntryMap entries_;
    mutable std::size_t accounted_bytes_ = 0;
    // Ordered index contains exactly one (deadline, binary key) per volatile
    // entry. All mutation paths maintain it; scans never depend on hash
    // iterators surviving a rehash, erase or MSET clone-and-swap.
    using ExpiryKey = std::pair<WallTimePoint, std::string>;
    mutable std::set<ExpiryKey> expiry_index_;
};

} // namespace redisdb::storage
