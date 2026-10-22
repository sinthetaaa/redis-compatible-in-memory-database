#include "redisdb/storage/Database.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <variant>

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
