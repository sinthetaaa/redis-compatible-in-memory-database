#include "redisdb/storage/Database.hpp"
#include "redisdb/storage/MemoryAccounting.hpp"

#include <algorithm>
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

Database::Database(MemoryOptions options) noexcept
    : clock_(production_clock()), memory_options_(options) {}
Database::Database(const Clock& clock, MemoryOptions options) noexcept
    : clock_(clock), memory_options_(options) {}

void Database::swap_state(Database& other) noexcept {
    entries_.swap(other.entries_);
    expiry_index_.swap(other.expiry_index_);
    lru_.swap(other.lru_);
    std::swap(accounted_bytes_, other.accounted_bytes_);
}

std::unique_ptr<Database> Database::clone_state() const {
    auto clone = std::make_unique<Database>(clock_, memory_options_);
    clone->entries_ = entries_;
    clone->expiry_index_ = expiry_index_;
    clone->lru_ = lru_;
    for (auto it = clone->lru_.begin(); it != clone->lru_.end(); ++it) {
        clone->entries_.at(*it).lru_position = it;
    }
    clone->accounted_bytes_ = accounted_bytes_;
    return clone;
}

Database::Transaction::Transaction(Database& database) : database_(database) {
    if (database_.transaction_active_) {
        throw std::logic_error("nested database transaction");
    }
    original_ = database_.clone_state();
    database_.swap_state(*original_);
    database_.transaction_active_ = true;
}

Database::Transaction::~Transaction() noexcept {
    if (!committed_) {
        database_.swap_state(*original_);
    }
    database_.transaction_active_ = false;
}

void Database::Transaction::commit() noexcept { committed_ = true; }

CommittedEffects Database::Transaction::effects() const {
    CommittedEffects result;
    for (const auto& [key, entry] : original_->entries_) {
        (void)entry;
        if (!database_.entries_.contains(key)) {
            result.entries.push_back({key, std::nullopt});
        }
    }
    for (const auto& [key, entry] : database_.entries_) {
        const auto old = original_->entries_.find(key);
        if (old == original_->entries_.end() || old->second.value != entry.value ||
            old->second.expires_at != entry.expires_at) {
            result.entries.push_back({key, entry});
        }
    }
    std::sort(result.entries.begin(), result.entries.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    result.has_lru = database_.memory_options_.policy == EvictionPolicy::AllKeysLru;
    if (result.has_lru && !result.entries.empty()) {
        result.lru_order.assign(database_.lru_.begin(), database_.lru_.end());
    }
    return result;
}

bool Database::has_due_expiration() const noexcept {
    return !expiry_index_.empty() && expiry_index_.begin()->first <= clock_.wall_now();
}

void Database::replay_effects(const CommittedEffects& effects) {
    const auto saved_limit = memory_options_.max_bytes;
    memory_options_.max_bytes.reset();
    try {
        EntryMap replacements;
        for (const auto& effect : effects.entries) {
            if (effect.replacement) {
                Entry entry{effect.replacement->value, effect.replacement->expires_at};
                replacements.emplace(effect.key, std::move(entry));
            } else {
                const auto found = entries_.find(effect.key);
                if (found != entries_.end()) {
                    erase_entry(found);
                }
            }
        }
        commit_entries(std::move(replacements));
        if (memory_options_.policy == EvictionPolicy::AllKeysLru) {
            std::list<std::string> order;
            if (effects.has_lru) {
                if (effects.lru_order.size() != entries_.size()) {
                    throw std::runtime_error("AOF LRU key count mismatch");
                }
                std::set<std::string_view> unique;
                for (const auto& key : effects.lru_order) {
                    if (!entries_.contains(key) || !unique.insert(key).second) {
                        throw std::runtime_error("AOF invalid LRU key");
                    }
                    order.push_back(key);
                }
            } else {
                for (const auto& [key, entry] : entries_) {
                    (void)entry;
                    order.push_back(key);
                }
                order.sort(); // policy changed from noeviction: deterministic cold start
            }
            lru_.swap(order);
            for (auto it = lru_.begin(); it != lru_.end(); ++it) {
                entries_.at(*it).lru_position = it;
            }
        }
    } catch (...) {
        memory_options_.max_bytes = saved_limit;
        throw;
    }
    memory_options_.max_bytes = saved_limit;
}

void Database::visit_snapshot(
    const std::function<void(const std::string&, const Entry&)>& visitor) const {
    std::vector<std::string_view> keys;
    keys.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        (void)entry;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    for (const auto key : keys) {
        const auto found = entries_.find(key);
        visitor(found->first, found->second);
    }
}

CommittedEffects Database::snapshot_order() const {
    CommittedEffects result;
    result.has_lru = memory_options_.policy == EvictionPolicy::AllKeysLru;
    if (result.has_lru) {
        result.lru_order.assign(lru_.begin(), lru_.end());
    }
    return result;
}

void Database::restore_snapshot_entry(EntryEffect effect) {
    if (!effect.replacement || entries_.contains(effect.key)) {
        throw std::runtime_error("snapshot duplicate key or tombstone");
    }
    EntryMap staged;
    staged.emplace(std::move(effect.key), std::move(*effect.replacement));
    const auto limit = memory_options_.max_bytes;
    memory_options_.max_bytes.reset();
    try {
        commit_entries(std::move(staged));
    } catch (...) {
        memory_options_.max_bytes = limit;
        throw;
    }
    memory_options_.max_bytes = limit;
}

void Database::finish_recovery() {
    (void)sweep_expired(std::numeric_limits<std::size_t>::max());
    if (memory_options_.max_bytes && accounted_bytes_ > *memory_options_.max_bytes) {
        throw MemoryLimitError{}; // never silently evict committed state at startup
    }
}

void Database::touch(EntryMap::const_iterator found) const noexcept {
    if (memory_options_.policy == EvictionPolicy::AllKeysLru) {
        lru_.splice(lru_.end(), lru_, found->second.lru_position);
    }
}

void Database::commit_entries(EntryMap staged) {
    namespace ma = memory_accounting;
    static_assert(std::is_nothrow_move_assignable_v<Entry>);
    auto next_total = accounted_bytes_;
    auto required_entries = entries_.size();
    std::size_t incoming = 0;
    std::set<ExpiryKey, ExpiryLess> staged_expiry;
    std::list<std::string> staged_lru;
    for (auto& [key, entry] : staged) {
        const auto old = entries_.find(key);
        if (old != entries_.end()) {
            next_total = ma::removal_total(next_total, old->second.accounted_charge);
        } else {
            required_entries = ma::checked_add(required_entries, 1);
        }
        entry.accounted_charge = ma::entry_charge(key, entry);
        incoming = ma::checked_add(incoming, entry.accounted_charge);
        if (entry.expires_at) {
            staged_expiry.emplace(*entry.expires_at, key);
        }
        if (memory_options_.policy == EvictionPolicy::AllKeysLru) {
            staged_lru.push_back(key);
            entry.lru_position = std::prev(staged_lru.end());
        }
    }
    // Stable ordering for the simultaneous MSET access: binary-key ascending.
    staged_lru.sort();
    next_total = ma::checked_add(next_total, incoming);
    std::vector<EntryMap::iterator> victims;
    // Reserve before retaining iterators or touching live data. Node transfer
    // below cannot rehash or allocate (identical standard allocators/hash).
    // The private map keeps its default max_load_factor of 1. Avoid reserve
    // on every replacement: reserve may otherwise shrink/rehash the table.
    if (required_entries > entries_.bucket_count()) {
        entries_.reserve(required_entries);
    }
    if (memory_options_.max_bytes && next_total > *memory_options_.max_bytes) {
        const auto limit = *memory_options_.max_bytes;
        if (incoming > limit) {
            throw MemoryLimitError{}; // destinations alone cannot fit
        }
        const auto now = clock_.wall_now();
        for (auto it = expiry_index_.begin();
             it != expiry_index_.end() && it->first <= now && next_total > limit; ++it) {
            if (!staged.contains(it->second)) {
                auto found = entries_.find(it->second);
                victims.push_back(found);
                next_total = ma::removal_total(next_total, found->second.accounted_charge);
            }
        }
        if (next_total > limit && memory_options_.policy == EvictionPolicy::AllKeysLru) {
            for (const auto& key : lru_) {
                if (staged.contains(key)) {
                    continue;
                }
                auto found = entries_.find(key);
                // All eligible expired entries were selected above before live LRU.
                if (found->second.expires_at && *found->second.expires_at <= now) {
                    continue;
                }
                victims.push_back(found);
                next_total = ma::removal_total(next_total, found->second.accounted_charge);
                if (next_total <= limit) {
                    break;
                }
            }
        }
        if (next_total > limit) {
            throw MemoryLimitError{};
        }
    }
    // Commit boundary: all potentially throwing allocation/validation is done.
    // Heterogeneous index erasure avoids allocating key copies during teardown.
    for (auto victim : victims) {
        erase_entry(victim);
    }
    for (const auto& [key, entry] : staged) {
        (void)entry;
        const auto old = entries_.find(key);
        if (old != entries_.end()) {
            erase_entry(old);
        }
    }
    while (!staged.empty()) {
        (void)entries_.insert(staged.extract(staged.begin()));
    }
    expiry_index_.merge(staged_expiry);
    lru_.splice(lru_.end(), staged_lru);
    accounted_bytes_ = next_total;
}

void Database::insert_new(std::string key, Entry staged) {
    EntryMap replacements;
    replacements.emplace(std::move(key), std::move(staged));
    commit_entries(std::move(replacements));
}

void Database::replace_value(EntryMap::iterator found, Value staged) {
    insert_new(found->first, Entry{std::move(staged), found->second.expires_at});
}

void Database::erase_entry(EntryMap::iterator found) const {
    const auto next_total =
        memory_accounting::removal_total(accounted_bytes_, found->second.accounted_charge);
    if (found->second.expires_at) {
        const auto indexed = expiry_index_.find(
            std::pair{*found->second.expires_at, std::string_view(found->first)});
        if (indexed != expiry_index_.end()) {
            expiry_index_.erase(indexed);
        }
    }
    if (memory_options_.policy == EvictionPolicy::AllKeysLru) {
        lru_.erase(found->second.lru_position);
    }
    entries_.erase(found);
    accounted_bytes_ = next_total;
}

bool Database::expire_if_needed(std::string_view key) const {
    const auto found = entries_.find(key);
    if (found == entries_.end() || !found->second.expires_at.has_value() ||
        *found->second.expires_at > clock_.wall_now()) {
        return false;
    }
    erase_entry(found);
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
    const bool created = !entries_.contains(key);
    insert_new(std::move(key), Entry{Value{StringValue{std::move(bytes)}}});
    return created;
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

    insert_new(std::move(key), std::move(replacement));
    return SetWithExpiryResult::Set;
}

void Database::set_many(const std::vector<std::pair<std::string, std::string>>& pairs) {
    EntryMap staged;
    for (const auto& [key, bytes] : pairs) {
        staged.insert_or_assign(key, Entry{Value{StringValue{bytes}}});
    }
    commit_entries(std::move(staged));
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
        const auto length = owned_value.size();
        insert_new(std::move(owned_key), Entry{Value{StringValue{std::move(owned_value)}}});
        return length;
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
    replace_value(found, std::move(next));
    return length;
}

std::size_t Database::string_length(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return 0;
    }
    touch(found);
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
        replace_value(found, std::move(replacement));
    } else {
        insert_new(std::string(key), Entry{std::move(replacement)});
    }
    return {IntegerMutationError::None, next};
}

bool Database::set_expiration(std::string_view key, WallTimePoint deadline) {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    insert_new(found->first, Entry{found->second.value, deadline});
    return true;
}

std::optional<WallTimePoint> Database::expiration(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    touch(found);
    return found->second.expires_at;
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
    const auto delta =
        memory_accounting::checked_add(memory_accounting::kExpiryFixedBytes, found->first.size());
    if (delta > found->second.accounted_charge) {
        throw std::logic_error("invalid expiration charge");
    }
    const auto charge = found->second.accounted_charge - delta;
    const auto candidate = memory_accounting::replacement_total(
        accounted_bytes_, found->second.accounted_charge, charge);
    expiry_index_.erase(
        expiry_index_.find(std::pair{*found->second.expires_at, std::string_view(found->first)}));
    found->second.expires_at.reset();
    found->second.accounted_charge = charge;
    accounted_bytes_ = candidate;
    touch(found);
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
        erase_entry(found);
        return ExpireResult::Missing;
    }
    if (amount <= 0) {
        erase_entry(found);
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
    insert_new(found->first, Entry{found->second.value, deadline});
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
        touch(found);
        return -1;
    }
    const auto now = clock_.wall_now();
    const auto deadline = *found->second.expires_at;
    if (deadline <= now) {
        erase_entry(found);
        return -2;
    }

    touch(found);
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
    touch(found);
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
        insert_new(std::move(key), Entry{std::move(staged)});
    } else {
        // Variant move assignment is noexcept, and the TTL index is untouched.
        replace_value(found, std::move(staged));
    }
    return count;
}

std::size_t Database::list_length(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return 0;
    }
    touch(found);
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
        erase_entry(found);
    } else {
        const auto delta = memory_accounting::checked_add(memory_accounting::kListElementFixedBytes,
                                                          result.size());
        if (delta > found->second.accounted_charge) {
            throw std::logic_error("invalid list charge");
        }
        const auto charge = found->second.accounted_charge - delta;
        const auto candidate = memory_accounting::replacement_total(
            accounted_bytes_, found->second.accounted_charge, charge);
        if (left) {
            list->elements.pop_front();
        } else {
            list->elements.pop_back();
        }
        found->second.accounted_charge = charge;
        accounted_bytes_ = candidate;
        touch(found);
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
    touch(found);
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
        insert_new(std::move(key), Entry{std::move(staged)});
    } else {
        // Keep existing TTL and index unchanged on hash-field updates.
        replace_value(found, std::move(staged));
    }
    return inserted;
}

std::optional<std::string> Database::hash_get(std::string_view key, std::string_view field) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    touch(found);
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
    touch(found);
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
    touch(found);
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
        touch(found);
        return 0;
    }
    if (next.fields.empty()) {
        erase_entry(found);
    } else {
        replace_value(found, Value{HashValue{std::move(next)}});
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
    touch(found);
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

std::size_t Database::set_add(std::string key, const std::vector<std::string>& members) {
    if (members.empty()) {
        throw std::invalid_argument("set add requires at least one member");
    }
    (void)expire_if_needed(key);
    const auto found = entries_.find(std::string_view(key));
    SetValue next;
    if (found != entries_.end()) {
        const auto* previous = std::get_if<SetValue>(&found->second.value);
        if (previous == nullptr) {
            throw WrongTypeError{};
        }
        next = *previous;
    }
    std::size_t inserted = 0;
    for (const auto& member : members) {
        if (next.members.insert(member).second) {
            ++inserted;
        }
    }
    if (inserted == 0) {
        touch(found);
        return 0;
    }
    Value staged = SetValue{std::move(next)};
    if (found == entries_.end()) {
        insert_new(std::move(key), Entry{std::move(staged)});
    } else {
        replace_value(found, std::move(staged));
    }
    return inserted;
}

std::size_t Database::set_remove(std::string_view key, const std::vector<std::string>& members) {
    if (members.empty()) {
        throw std::invalid_argument("set remove requires at least one member");
    }
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return 0;
    }
    const auto* set = std::get_if<SetValue>(&found->second.value);
    if (set == nullptr) {
        throw WrongTypeError{};
    }
    SetValue next = *set;
    std::size_t removed = 0;
    for (const auto& member : members) {
        removed += next.members.erase(member);
    }
    if (removed == 0) {
        touch(found);
        return 0;
    }
    if (next.members.empty()) {
        erase_entry(found);
    } else {
        replace_value(found, Value{SetValue{std::move(next)}});
    }
    return removed;
}

bool Database::set_contains(std::string_view key, std::string_view member) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    touch(found);
    const auto* set = std::get_if<SetValue>(&found->second.value);
    if (set == nullptr) {
        throw WrongTypeError{};
    }
    return set->members.contains(std::string(member));
}

std::size_t Database::set_cardinality(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return 0;
    }
    touch(found);
    const auto* set = std::get_if<SetValue>(&found->second.value);
    if (set == nullptr) {
        throw WrongTypeError{};
    }
    return set->members.size();
}

std::optional<std::vector<std::string>> Database::set_members(std::string_view key,
                                                              std::size_t max_reply_bytes) const {
    // The empty RESP2 array *0\r\n occupies four bytes.
    if (max_reply_bytes < 4) {
        return std::nullopt;
    }
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::vector<std::string>{};
    }
    touch(found);
    const auto* set = std::get_if<SetValue>(&found->second.value);
    if (set == nullptr) {
        throw WrongTypeError{};
    }
    const std::size_t count = set->members.size();
    // Even an empty bulk string takes six wire bytes: $0\r\n\r\n.
    if (count > max_reply_bytes / 6) {
        return std::nullopt;
    }
    std::size_t total = 3 + std::to_string(count).size();
    if (total > max_reply_bytes) {
        return std::nullopt;
    }
    for (const auto& member : set->members) {
        const std::size_t overhead = 5 + std::to_string(member.size()).size();
        if (overhead > max_reply_bytes - total ||
            member.size() > max_reply_bytes - total - overhead) {
            return std::nullopt;
        }
        total += overhead + member.size();
    }
    std::vector<std::string> result;
    result.reserve(count);
    for (const auto& member : set->members) {
        result.push_back(member);
    }
    return result;
}

std::optional<std::string> Database::get(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    touch(found);
    const auto* string_value = std::get_if<StringValue>(&found->second.value);
    if (string_value == nullptr) {
        throw WrongTypeError{};
    }
    return string_value->bytes;
}

bool Database::exists(std::string_view key) const {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    touch(found);
    return true;
}

bool Database::erase(std::string_view key) {
    (void)expire_if_needed(key);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    erase_entry(found);
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
            erase_entry(found);
            ++result.deleted;
            ++result.inspected;
            continue;
        }
        expiry_index_.erase(earliest);
        ++result.inspected;
    }
    return result;
}

} // namespace redisdb::storage
