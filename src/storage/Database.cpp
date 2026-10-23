#include "redisdb/storage/Database.hpp"

#include <charconv>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
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
    expiry_index_.erase(ExpiryKey{*found->second.expires_at, found->first});
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
        Value replacement = StringValue{std::move(bytes)};
        if (found->second.expires_at.has_value()) {
            // Index removal precedes the no-throw value move. Any key-copy
            // allocation failure leaves the old entry completely untouched.
            expiry_index_.erase(ExpiryKey{*found->second.expires_at, found->first});
        }
        found->second.value = std::move(replacement);
        found->second.expires_at.reset();
        return false;
    }
    const auto inserted =
        entries_.try_emplace(std::move(key), Entry{Value{StringValue{std::move(bytes)}}});
    return inserted.second;
}

SetWithExpiryResult Database::set_with_expiration(std::string key, std::string bytes,
                                                  std::int64_t amount, bool milliseconds) {
    using NativeDuration = WallTimePoint::duration;
    using Rep = NativeDuration::rep;
    using Period = NativeDuration::period;
    static_assert(std::is_integral_v<Rep> && std::is_signed_v<Rep>);
    static_assert(std::numeric_limits<Rep>::digits == 63);
    static_assert(Period::num == 1 && Period::den % 1000 == 0);
    constexpr std::int64_t TicksPerMillisecond = Period::den / 1000;
    constexpr std::int64_t TicksPerSecond = Period::den;
    constexpr std::int64_t Max = std::numeric_limits<Rep>::max();
    if (amount <= 0) {
        return SetWithExpiryResult::OutOfRange;
    }
    const auto now = clock_.wall_now();
    const std::int64_t scale = milliseconds ? TicksPerMillisecond : TicksPerSecond;
    if (amount > Max / scale) {
        return SetWithExpiryResult::OutOfRange;
    }
    const std::int64_t delta = amount * scale;
    const std::int64_t now_ticks = now.time_since_epoch().count();
    if (now_ticks > Max - delta) {
        return SetWithExpiryResult::OutOfRange;
    }
    const WallTimePoint deadline{NativeDuration{now_ticks + delta}};
    Entry replacement{Value{StringValue{std::move(bytes)}}, deadline};

    // Stage index allocation before touching the authoritative entry.
    // Roll back the new index record if insertion of a new entry throws.
    const auto found = entries_.find(std::string_view(key));
    std::optional<ExpiryKey> previous_record;
    if (found != entries_.end() && found->second.expires_at.has_value() &&
        *found->second.expires_at != deadline) {
        previous_record.emplace(*found->second.expires_at, found->first);
    }
    const auto indexed = expiry_index_.emplace(deadline, key);
    if (found != entries_.end()) {
        found->second = std::move(replacement);
        if (previous_record.has_value()) {
            expiry_index_.erase(*previous_record);
        }
    } else {
        try {
            (void)entries_.try_emplace(std::move(key), std::move(replacement));
        } catch (...) {
            if (indexed.second) {
                expiry_index_.erase(indexed.first);
            }
            throw;
        }
    }
    return SetWithExpiryResult::Set;
}

void Database::set_many(const std::vector<std::pair<std::string, std::string>>& pairs) {
    // Both authoritative structures are fully staged before commit.
    // Duplicate MSET keys are last-wins, while prior TTL metadata is cleared.
    auto next = entries_;
    auto next_expiry = expiry_index_;
    for (const auto& [key, bytes] : pairs) {
        const auto existing = next.find(key);
        if (existing != next.end() && existing->second.expires_at.has_value()) {
            next_expiry.erase(ExpiryKey{*existing->second.expires_at, existing->first});
        }
        next.insert_or_assign(key, Entry{Value{StringValue{bytes}}});
    }
    entries_.swap(next);
    expiry_index_.swap(next_expiry);
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

    const auto* string_value = std::get_if<StringValue>(&found->second.value);
    if (string_value == nullptr) {
        throw WrongTypeError{};
    }
    const std::string& existing = string_value->bytes;
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
    const auto* string_value = std::get_if<StringValue>(&found->second.value);
    if (string_value == nullptr) {
        throw WrongTypeError{};
    }
    return string_value->bytes.size();
}

IntegerMutationResult Database::adjust_integer(std::string_view key, std::int64_t operand,
                                               bool subtract) {
    (void)expire_if_needed(key);
    std::int64_t current = 0;
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
        const auto* string_value = std::get_if<StringValue>(&found->second.value);
        if (string_value == nullptr) {
            throw WrongTypeError{};
        }
        const auto value = parse_integer64(string_value->bytes);
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
    const auto previous = found->second.expires_at;
    std::optional<ExpiryKey> previous_record;
    if (previous.has_value() && *previous != deadline) {
        previous_record.emplace(*previous, found->first);
    }
    (void)expiry_index_.emplace(deadline, found->first);
    found->second.expires_at = deadline;
    if (previous_record.has_value()) {
        expiry_index_.erase(*previous_record);
    }
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
    if (!found->second.expires_at.has_value()) {
        return false;
    }
    expiry_index_.erase(ExpiryKey{*found->second.expires_at, found->first});
    found->second.expires_at.reset();
    return true;
}

ExpireResult Database::expire_after(std::string_view key, std::int64_t amount, bool milliseconds) {
    using NativeDuration = WallTimePoint::duration;
    using Rep = NativeDuration::rep;
    using Period = NativeDuration::period;
    static_assert(std::is_integral_v<Rep> && std::is_signed_v<Rep>);
    static_assert(std::numeric_limits<Rep>::digits == 63);
    static_assert(Period::num == 1 && Period::den % 1000 == 0);
    constexpr std::int64_t TicksPerMillisecond = Period::den / 1000;
    constexpr std::int64_t TicksPerSecond = Period::den;
    const auto now = clock_.wall_now();
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return ExpireResult::Missing;
    }
    const auto previous = found->second.expires_at;
    if (previous.has_value() && *previous <= now) {
        expiry_index_.erase(ExpiryKey{*previous, found->first});
        entries_.erase(found);
        return ExpireResult::Missing;
    }
    if (amount <= 0) {
        if (previous.has_value()) {
            expiry_index_.erase(ExpiryKey{*previous, found->first});
        }
        entries_.erase(found);
        return ExpireResult::Deleted;
    }
    const std::int64_t scale = milliseconds ? TicksPerMillisecond : TicksPerSecond;
    constexpr std::int64_t Max = std::numeric_limits<Rep>::max();
    if (amount > Max / scale) {
        return ExpireResult::OutOfRange;
    }
    const std::int64_t delta = amount * scale;
    const std::int64_t now_ticks = now.time_since_epoch().count();
    if (now_ticks > Max - delta) {
        return ExpireResult::OutOfRange;
    }
    const WallTimePoint deadline{NativeDuration{now_ticks + delta}};
    std::optional<ExpiryKey> previous_record;
    if (previous.has_value() && *previous != deadline) {
        previous_record.emplace(*previous, found->first);
    }
    (void)expiry_index_.emplace(deadline, found->first);
    found->second.expires_at = deadline;
    if (previous_record.has_value()) {
        expiry_index_.erase(*previous_record);
    }
    return ExpireResult::Set;
}

std::int64_t Database::remaining_ttl(std::string_view key, bool milliseconds) const {
    // Do not call expiration() and sample the clock again: a single command
    // must classify existence and compute its remaining time consistently.
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return -2;
    }
    if (!found->second.expires_at.has_value()) {
        return -1;
    }
    const auto now = clock_.wall_now();
    const auto deadline = *found->second.expires_at;
    if (deadline <= now) {
        expiry_index_.erase(ExpiryKey{deadline, found->first});
        entries_.erase(found);
        return -2;
    }

    using Duration = WallTimePoint::duration;
    using Rep = Duration::rep;
    using Period = Duration::period;
    static_assert(std::is_integral_v<Rep> && std::is_signed_v<Rep>);
    static_assert(std::numeric_limits<Rep>::digits == 63);
    static_assert(Period::num == 1 && Period::den % 1000 == 0);
    // Subtracting signed duration reps could overflow across the epoch.
    // For deadline > now, unsigned modular subtraction gives the exact
    // nonnegative 0..UINT64_MAX tick interval without signed overflow.
    const auto high = static_cast<std::uint64_t>(deadline.time_since_epoch().count());
    const auto low = static_cast<std::uint64_t>(now.time_since_epoch().count());
    const std::uint64_t ticks_remaining = high - low;
    constexpr std::uint64_t TicksPerSecond = Period::den;
    constexpr std::uint64_t TicksPerMillisecond = Period::den / 1000;
    const auto scale = milliseconds ? TicksPerMillisecond : TicksPerSecond;
    std::uint64_t units = ticks_remaining / scale;
    // Redis TTL seconds uses nearest-integer rounding, rather than floor.
    // Compare the remainder to half a second without adding to a large
    // duration and risking unsigned wraparound.
    if (!milliseconds && ticks_remaining % scale >= (scale + 1) / 2) {
        ++units;
    }
    // Every supported native clock has >=1000 ticks/ms; therefore even a
    // maximum uint64 interval becomes a representable signed millisecond TTL.
    static_assert(TicksPerMillisecond >= 1000);
    return static_cast<std::int64_t>(units);
}

StoredType Database::type_of(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return StoredType::None;
    }
    switch (found->second.value.index()) {
    case 0:
        return StoredType::String;
    case 1:
        return StoredType::List;
    case 2:
        return StoredType::Hash;
    case 3:
        return StoredType::Set;
    default:
        throw std::logic_error("unknown storage variant type");
    }
}

std::size_t Database::list_push(std::string key, const std::vector<std::string>& values,
                                bool left) {
    if (values.empty()) {
        throw std::invalid_argument("list push requires at least one element");
    }
    (void)expire_if_needed(key);
    const auto found = entries_.find(std::string_view(key));
    ListValue next;
    if (found != entries_.end()) {
        const auto* prior = std::get_if<ListValue>(&found->second.value);
        if (prior == nullptr) {
            throw WrongTypeError{};
        }
        next = *prior; // Stage a deep copy before touching the live entry.
    }
    for (const auto& bytes : values) {
        if (left) {
            next.elements.push_front(bytes);
        } else {
            next.elements.push_back(bytes);
        }
    }
    const std::size_t count = next.elements.size();
    Value staged = ListValue{std::move(next)};
    if (found == entries_.end()) {
        (void)entries_.try_emplace(std::move(key), Entry{std::move(staged)});
    } else {
        // Variant move assignment is noexcept, and the TTL index is untouched.
        found->second.value = std::move(staged);
    }
    return count;
}

std::size_t Database::list_length(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return 0;
    }
    const auto* list = std::get_if<ListValue>(&found->second.value);
    if (list == nullptr) {
        throw WrongTypeError{};
    }
    return list->elements.size();
}

std::optional<std::string> Database::list_pop(std::string_view key, bool left) {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    auto* list = std::get_if<ListValue>(&found->second.value);
    if (list == nullptr) {
        throw WrongTypeError{};
    }
    if (list->elements.empty()) {
        // A zero-element stored list is not generated by list_push. Treat it as
        // a missing element without synthesizing a string or changing TTL.
        return std::nullopt;
    }
    std::string result = left ? list->elements.front() : list->elements.back();
    if (list->elements.size() == 1) {
        // Allocate the index key before mutating either authoritative store.
        std::optional<ExpiryKey> expiry_record;
        if (found->second.expires_at.has_value()) {
            expiry_record.emplace(*found->second.expires_at, found->first);
        }
        if (expiry_record.has_value()) {
            expiry_index_.erase(*expiry_record);
        }
        entries_.erase(found);
    } else if (left) {
        list->elements.pop_front();
    } else {
        list->elements.pop_back();
    }
    return result;
}

std::optional<std::vector<std::string>> Database::list_range(std::string_view key,
                                                             std::int64_t start, std::int64_t stop,
                                                             std::size_t max_reply_bytes) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::vector<std::string>{};
    }
    const auto* list = std::get_if<ListValue>(&found->second.value);
    if (list == nullptr) {
        throw WrongTypeError{};
    }
    const std::size_t size = list->elements.size();
    if (size == 0) {
        return std::vector<std::string>{};
    }

    // Avoid signed negation of INT64_MIN and overflow in size/index arithmetic.
    const auto negative_magnitude = [](std::int64_t index) -> std::uint64_t {
        return static_cast<std::uint64_t>(-(index + 1)) + 1U;
    };
    std::size_t first = 0;
    if (start >= 0) {
        const auto offset = static_cast<std::uint64_t>(start);
        if (offset >= size) {
            return std::vector<std::string>{};
        }
        first = static_cast<std::size_t>(offset);
    } else {
        const auto magnitude = negative_magnitude(start);
        first = magnitude >= size ? 0 : size - static_cast<std::size_t>(magnitude);
    }

    std::size_t last = 0;
    if (stop >= 0) {
        const auto offset = static_cast<std::uint64_t>(stop);
        last = offset >= size ? size - 1 : static_cast<std::size_t>(offset);
    } else {
        const auto magnitude = negative_magnitude(stop);
        if (magnitude > size) {
            return std::vector<std::string>{};
        }
        last = size - static_cast<std::size_t>(magnitude);
    }
    if (first > last) {
        return std::vector<std::string>{};
    }
    const std::size_t count = last - first + 1;
    // Each RESP2 bulk item has at least six bytes ($0\r\n\r\n).
    if (count > max_reply_bytes / 6) {
        return std::nullopt;
    }
    std::size_t total = 3 + std::to_string(count).size();
    if (total > max_reply_bytes) {
        return std::nullopt;
    }
    std::vector<std::string> result;
    result.reserve(count);
    auto it = list->elements.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(first));
    for (std::size_t i = 0; i < count; ++i, ++it) {
        const std::size_t bytes = it->size();
        const std::size_t overhead = 5 + std::to_string(bytes).size();
        if (overhead > max_reply_bytes - total || bytes > max_reply_bytes - total - overhead) {
            return std::nullopt;
        }
        total += overhead + bytes;
        result.push_back(*it);
    }
    return result;
}

std::size_t Database::hash_set(std::string key,
                               const std::vector<std::pair<std::string, std::string>>& fields) {
    if (fields.empty()) {
        throw std::invalid_argument("hash set requires at least one field/value pair");
    }
    (void)expire_if_needed(key);
    const auto found = entries_.find(std::string_view(key));
    HashValue next;
    if (found != entries_.end()) {
        const auto* previous = std::get_if<HashValue>(&found->second.value);
        if (previous == nullptr) {
            throw WrongTypeError{};
        }
        next = *previous; // Stage the entire hash before touching the live entry.
    }
    std::size_t inserted = 0;
    for (const auto& [field, value] : fields) {
        const auto result = next.fields.insert_or_assign(field, value);
        if (result.second) {
            ++inserted;
        }
    }
    Value staged = HashValue{std::move(next)};
    if (found == entries_.end()) {
        (void)entries_.try_emplace(std::move(key), Entry{std::move(staged)});
    } else {
        // Keep existing TTL and index unchanged on hash-field updates.
        found->second.value = std::move(staged);
    }
    return inserted;
}

std::optional<std::string> Database::hash_get(std::string_view key, std::string_view field) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    const auto* hash = std::get_if<HashValue>(&found->second.value);
    if (hash == nullptr) {
        throw WrongTypeError{};
    }
    const auto element = hash->fields.find(std::string(field));
    if (element == hash->fields.end()) {
        return std::nullopt;
    }
    return element->second;
}

bool Database::hash_exists(std::string_view key, std::string_view field) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    const auto* hash = std::get_if<HashValue>(&found->second.value);
    if (hash == nullptr) {
        throw WrongTypeError{};
    }
    return hash->fields.contains(std::string(field));
}

std::size_t Database::hash_length(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return 0;
    }
    const auto* hash = std::get_if<HashValue>(&found->second.value);
    if (hash == nullptr) {
        throw WrongTypeError{};
    }
    return hash->fields.size();
}

std::size_t Database::hash_delete(std::string_view key, const std::vector<std::string>& fields) {
    if (fields.empty()) {
        throw std::invalid_argument("hash delete requires at least one field");
    }
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return 0;
    }
    const auto* hash = std::get_if<HashValue>(&found->second.value);
    if (hash == nullptr) {
        throw WrongTypeError{};
    }
    HashValue next = *hash; // Preserve original if allocation fails.
    std::size_t removed = 0;
    for (const auto& field : fields) {
        removed += next.fields.erase(field);
    }
    if (removed == 0) {
        return 0;
    }
    if (next.fields.empty()) {
        // Stage index-record construction before modifying either store.
        std::optional<ExpiryKey> indexed;
        if (found->second.expires_at.has_value()) {
            indexed.emplace(*found->second.expires_at, found->first);
        }
        if (indexed.has_value()) {
            expiry_index_.erase(*indexed);
        }
        entries_.erase(found);
    } else {
        found->second.value = Value{HashValue{std::move(next)}};
    }
    return removed;
}

std::optional<std::vector<std::pair<std::string, std::string>>>
Database::hash_items(std::string_view key, std::size_t max_reply_bytes) const {
    // Even a zero-element RESP2 array needs *0\r\n (4 bytes).
    if (max_reply_bytes < 4) {
        return std::nullopt;
    }
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::vector<std::pair<std::string, std::string>>{};
    }
    const auto* hash = std::get_if<HashValue>(&found->second.value);
    if (hash == nullptr) {
        throw WrongTypeError{};
    }
    const std::size_t count = hash->fields.size();
    // Two bulk-string frames need at least 12 encoded bytes per hash field.
    if (count > max_reply_bytes / 12) {
        return std::nullopt;
    }
    std::size_t total = 3 + std::to_string(count * 2).size();
    if (total > max_reply_bytes) {
        return std::nullopt;
    }
    for (const auto& [field, value] : hash->fields) {
        for (const auto* part : {&field, &value}) {
            const std::size_t overhead = 5 + std::to_string(part->size()).size();
            if (overhead > max_reply_bytes - total ||
                part->size() > max_reply_bytes - total - overhead) {
                return std::nullopt;
            }
            total += overhead + part->size();
        }
    }
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(count);
    for (const auto& [field, value] : hash->fields) {
        result.emplace_back(field, value);
    }
    return result;
}

std::optional<std::string> Database::get(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    const auto* string_value = std::get_if<StringValue>(&found->second.value);
    if (string_value == nullptr) {
        throw WrongTypeError{};
    }
    return string_value->bytes;
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
    if (found->second.expires_at.has_value()) {
        expiry_index_.erase(ExpiryKey{*found->second.expires_at, found->first});
    }
    entries_.erase(found);
    return true;
}

Database::SweepResult Database::sweep_expired(std::size_t budget) {
    SweepResult result;
    if (budget == 0 || expiry_index_.empty()) {
        return result;
    }
    const auto now = clock_.wall_now();
    // No scan of persistent or future-deadline keys. Both inspected and
    // deleted counts are <= budget; work per turn is bounded by index cost.
    while (result.inspected < budget && !expiry_index_.empty()) {
        const auto earliest = expiry_index_.begin();
        if (earliest->first > now) {
            break;
        }
        // Protect against stale index records if a future writer is buggy.
        const auto found = entries_.find(earliest->second);
        if (found != entries_.end() && found->second.expires_at == earliest->first) {
            entries_.erase(found);
            ++result.deleted;
        }
        expiry_index_.erase(earliest);
        ++result.inspected;
    }
    return result;
}

} // namespace redisdb::storage
