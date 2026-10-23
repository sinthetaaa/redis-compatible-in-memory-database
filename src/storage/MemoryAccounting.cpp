#include "redisdb/storage/MemoryAccounting.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace redisdb::storage::memory_accounting {

std::size_t checked_add(std::size_t lhs, std::size_t rhs) {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        throw std::overflow_error("memory accounting addition overflow");
    }
    return lhs + rhs;
}

std::size_t checked_multiply(std::size_t lhs, std::size_t rhs) {
    if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
        throw std::overflow_error("memory accounting multiplication overflow");
    }
    return lhs * rhs;
}

std::size_t replacement_total(std::size_t current_total, std::size_t old_entry_charge,
                              std::size_t new_entry_charge) {
    if (old_entry_charge > current_total) {
        throw std::logic_error("memory accounting prior charge exceeds total");
    }
    return checked_add(current_total - old_entry_charge, new_entry_charge);
}

std::size_t removal_total(std::size_t current_total, std::size_t old_entry_charge) {
    if (old_entry_charge > current_total) {
        throw std::logic_error("memory accounting removal exceeds total");
    }
    return current_total - old_entry_charge;
}

std::size_t value_charge(const Value& value) {
    return std::visit(
        [](const auto& stored) -> std::size_t {
            using T = std::decay_t<decltype(stored)>;
            if constexpr (std::is_same_v<T, StringValue>) {
                return stored.bytes.size();
            } else if constexpr (std::is_same_v<T, ListValue>) {
                std::size_t result =
                    checked_multiply(stored.elements.size(), kListElementFixedBytes);
                for (const auto& element : stored.elements) {
                    result = checked_add(result, element.size());
                }
                return result;
            } else if constexpr (std::is_same_v<T, HashValue>) {
                std::size_t result = checked_multiply(stored.fields.size(), kHashFieldFixedBytes);
                for (const auto& [field, bytes] : stored.fields) {
                    result = checked_add(result, checked_add(field.size(), bytes.size()));
                }
                return result;
            } else {
                static_assert(std::is_same_v<T, SetValue>);
                std::size_t result = checked_multiply(stored.members.size(), kSetMemberFixedBytes);
                for (const auto& member : stored.members) {
                    result = checked_add(result, member.size());
                }
                return result;
            }
        },
        value);
}

std::size_t entry_charge(std::string_view key, const Entry& entry) {
    std::size_t result = checked_add(kKeyFixedBytes, key.size());
    result = checked_add(result, value_charge(entry.value));
    if (entry.expires_at.has_value()) {
        result = checked_add(result, checked_add(kExpiryFixedBytes, key.size()));
    }
    return result;
}

} // namespace redisdb::storage::memory_accounting
