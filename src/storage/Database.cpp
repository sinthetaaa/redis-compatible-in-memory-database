#include "redisdb/storage/Database.hpp"

#include <charconv>
#include <limits>
#include <set>
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
