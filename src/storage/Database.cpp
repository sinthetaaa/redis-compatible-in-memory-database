#include "redisdb/storage/Database.hpp"

#include <charconv>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace redisdb::storage {
namespace {
const Clock& production_clock() noexcept {
    static const SystemClock clock;
    return clock;
}
} // namespace

Database::Database() noexcept : clock_(production_clock()) {}
Database::Database(const Clock& clock) noexcept : clock_(clock) {}

bool Database::expire_if_needed(std::string_view key) const {
    const auto found = entries_.find(key);
    if (found == entries_.end() || !found->second.expires_at.has_value() ||
        *found->second.expires_at > clock_.wall_now()) {
        return false;
    }
    // Access-driven deletion: no global scan and no eviction of live keys.
    entries_.erase(found);
    return true;
}

std::optional<std::int64_t> parse_integer64(std::string_view text) noexcept {
    if (text.empty() || text.front() == '+') {
        return std::nullopt;
    }
    // Redis-style canonical integer bytes: no leading zeroes or negative zero.
    if ((text.size() > 1 && text.front() == '0') ||
        (text.size() > 1 && text.front() == '-' && text[1] == '0')) {
        return std::nullopt;
    }
    std::int64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return result;
}

bool Database::set(std::string key, std::string bytes) {
    (void)expire_if_needed(key);
    auto found = entries_.find(std::string_view(key));
    if (found != entries_.end()) {
        // Construct first; moving a string into the same variant alternative
        // cannot require allocation with the standard string allocator.
        Value replacement = StringValue{std::move(bytes)};
        found->second.value = std::move(replacement);
        // Redis SET (without future KEEPTTL option) replaces any old deadline.
        // The value is fully staged before metadata is cleared.
        found->second.expires_at.reset();
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

std::optional<std::size_t> Database::append(std::string_view key, std::string_view suffix,
                                            std::size_t max_value_bytes) {
    (void)expire_if_needed(key);
    auto found = entries_.find(key);
    if (found == entries_.end()) {
        if (suffix.size() > max_value_bytes) {
            return std::nullopt;
        }
        // Stage all owned bytes before modifying the authoritative map.
        std::string owned_key(key);
        std::string owned_value(suffix);
        const auto [it, inserted] = entries_.try_emplace(
            std::move(owned_key), Entry{Value{StringValue{std::move(owned_value)}}});
        (void)inserted;
        return std::get<StringValue>(it->second.value).bytes.size();
    }

    const std::string& existing = std::get<StringValue>(found->second.value).bytes;
    if (existing.size() > max_value_bytes || suffix.size() > max_value_bytes - existing.size()) {
        return std::nullopt;
    }

    // Prepare the complete value before committing; allocation failure
    // leaves the original entry unchanged, including when suffix is empty.
    std::string replacement = existing;
    replacement.append(suffix);
    const std::size_t length = replacement.size();
    Value next = StringValue{std::move(replacement)};
    found->second.value = std::move(next);
    return length;
}

std::size_t Database::string_length(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return 0;
    }
    return std::get<StringValue>(found->second.value).bytes.size();
}

IntegerMutationResult Database::adjust_integer(std::string_view key, std::int64_t operand,
                                               bool subtract) {
    (void)expire_if_needed(key);
    std::int64_t current = 0;
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
        const auto value = parse_integer64(std::get<StringValue>(found->second.value).bytes);
        if (!value.has_value()) {
            return {IntegerMutationError::InvalidInteger, 0};
        }
        current = *value;
    }

    constexpr auto Min = std::numeric_limits<std::int64_t>::min();
    constexpr auto Max = std::numeric_limits<std::int64_t>::max();
    if (subtract) {
        // Check without computing -operand, which is UB for INT64_MIN.
        if ((operand > 0 && current < Min + operand) || (operand < 0 && current > Max + operand)) {
            return {IntegerMutationError::Overflow, 0};
        }
    } else {
        if ((operand > 0 && current > Max - operand) || (operand < 0 && current < Min - operand)) {
            return {IntegerMutationError::Overflow, 0};
        }
    }

    // Fully stage the new bytes before any existing-key mutation. Preserve
    // existing deadline without re-entering SET (which clears deadlines and
    // would resample the clock/possibly invalidate the found iterator).
    const std::int64_t next = subtract ? current - operand : current + operand;
    Value replacement = StringValue{std::to_string(next)};
    if (found != entries_.end()) {
        found->second.value = std::move(replacement);
    } else {
        (void)entries_.try_emplace(std::string(key), Entry{std::move(replacement)});
    }
    return {IntegerMutationError::None, next};
}

bool Database::set_expiration(std::string_view key, WallTimePoint deadline) {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    found->second.expires_at = deadline;
    return true;
}

std::optional<WallTimePoint> Database::expiration(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    return found == entries_.end() ? std::nullopt : found->second.expires_at;
}

bool Database::clear_expiration(std::string_view key) {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    const bool was_set = found->second.expires_at.has_value();
    found->second.expires_at.reset();
    return was_set;
}

std::optional<std::string> Database::get(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return std::get<StringValue>(found->second.value).bytes;
}

bool Database::exists(std::string_view key) const {
    (void)expire_if_needed(key);
    return entries_.find(key) != entries_.end();
}

bool Database::erase(std::string_view key) {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    entries_.erase(found);
    return true;
}

} // namespace redisdb::storage
