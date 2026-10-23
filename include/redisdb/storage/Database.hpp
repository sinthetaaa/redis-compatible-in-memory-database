#pragma once

#include "redisdb/storage/Entry.hpp"

#include <cstddef>
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

// Authoritative, process-local, binary-safe keyspace.
// The caller serializes access; this class is not independently synchronized.
// No TTL, eviction, persistence or protocol parsing is implemented here.
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
