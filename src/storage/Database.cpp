#include "redisdb/storage/Database.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace redisdb::storage {

bool Database::set(std::string key, std::string bytes) {
    auto found = entries_.find(std::string_view(key));
    if (found != entries_.end()) {
        // Construct first; moving a string into the same variant alternative
        // cannot require allocation with the standard string allocator.
        Value replacement = StringValue{std::move(bytes)};
        found->second.value = std::move(replacement);
        return false;
    }

    const auto inserted =
        entries_.try_emplace(std::move(key), Entry{Value{StringValue{std::move(bytes)}}});
    return inserted.second;
}

void Database::set_many(const std::vector<std::pair<std::string, std::string>>& pairs) {
    // Construct the whole next state before changing the authoritative map.
    // Allocation failure during cloning or replacement leaves it untouched.
    auto next = entries_;
    for (const auto& [key, bytes] : pairs) {
        next.insert_or_assign(key, Entry{Value{StringValue{bytes}}});
    }
    entries_.swap(next);
}

std::optional<std::string> Database::get(std::string_view key) const {
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return std::get<StringValue>(found->second.value).bytes;
}

bool Database::exists(std::string_view key) const { return entries_.find(key) != entries_.end(); }

bool Database::erase(std::string_view key) {
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    entries_.erase(found);
    return true;
}

} // namespace redisdb::storage
