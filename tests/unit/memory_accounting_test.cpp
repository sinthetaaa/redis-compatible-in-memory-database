#include "redisdb/storage/MemoryAccounting.hpp"

#include <cstddef>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using redisdb::storage::Entry;
using redisdb::storage::HashValue;
using redisdb::storage::ListValue;
using redisdb::storage::SetValue;
using redisdb::storage::StringValue;
using redisdb::storage::Value;
using redisdb::storage::WallTimePoint;
using redisdb::storage::memory_accounting::checked_add;
using redisdb::storage::memory_accounting::checked_multiply;
using redisdb::storage::memory_accounting::entry_charge;
using redisdb::storage::memory_accounting::value_charge;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Callable> void require_overflow(Callable&& callable, const char* message) {
    try {
        callable();
    } catch (const std::overflow_error&) {
        return;
    }
    throw std::runtime_error(message);
}

void test_frozen_coefficients_and_binary_strings() {
    namespace ma = redisdb::storage::memory_accounting;
    static_assert(ma::kKeyFixedBytes == 64);
    static_assert(ma::kListElementFixedBytes == 48);
    static_assert(ma::kHashFieldFixedBytes == 80);
    static_assert(ma::kSetMemberFixedBytes == 56);
    static_assert(ma::kExpiryFixedBytes == 48);
    require(entry_charge("", Entry{Value{StringValue{""}}}) == 64,
            "empty string entry must retain fixed key charge");
    const std::string key("k\0", 2);
    const std::string bytes("A\0\x80\xff", 4);
    const Entry record{Value{StringValue{bytes}}};
    require(value_charge(record.value) == 4, "binary bytes must be counted precisely");
    require(entry_charge(key, record) == 64 + 2 + 4, "binary key/value accounting");
}

void test_list_estimator() {
    ListValue list;
    list.elements.push_back("");
    list.elements.push_back(std::string("Z\0", 2));
    const Entry record{Value{std::move(list)}};
    require(value_charge(record.value) == 2 * 48 + 2, "list payload and two node charges");
    require(entry_charge("abc", record) == 64 + 3 + 2 * 48 + 2, "list entry reference formula");
}

void test_hash_estimator_order_independent() {
    HashValue hash;
    hash.fields.insert_or_assign("a", "xy");
    hash.fields.insert_or_assign("bb", std::string("\0", 1));
    hash.fields.insert_or_assign("a", "xy"); // overwrite must not add a field
    const Entry record{Value{std::move(hash)}};
    require(value_charge(record.value) == 2 * 80 + (1 + 2) + (2 + 1),
            "hash field/value independent reference formula");
    require(entry_charge("h", record) == 64 + 1 + 2 * 80 + 6, "hash key overhead and exact bytes");
}

void test_set_uniqueness() {
    SetValue set;
    set.members.insert("x");
    set.members.insert("x");
    set.members.insert(std::string("x\0", 2));
    const Entry record{Value{std::move(set)}};
    require(value_charge(record.value) == 2 * 56 + 3, "unique binary-safe set membership");
    require(entry_charge("s", record) == 64 + 1 + 2 * 56 + 3,
            "set entry independent reference formula");
}

void test_expiration_duplicate_key_charge() {
    const Entry no_expiry{Value{StringValue{"d"}}};
    const Entry expiry{Value{StringValue{"d"}}, WallTimePoint{}};
    require(entry_charge("abc", no_expiry) == 64 + 3 + 1, "no TTL must not charge expiry index");
    require(entry_charge("abc", expiry) == 64 + 3 + 1 + 48 + 3,
            "TTL must charge index node and duplicated key bytes");
    require(entry_charge("", Entry{Value{StringValue{""}}, WallTimePoint{}}) == 64 + 48,
            "empty key with expiry charges the fixed expiry node");
}

void test_checked_arithmetic() {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    require(checked_add(maximum, 0) == maximum, "no false positive on add zero");
    require(checked_multiply(maximum, 0) == 0, "no false positive on multiply zero");
    require(checked_multiply(maximum, 1) == maximum, "no false positive on multiply one");
    require_overflow([&] { (void)checked_add(maximum, 1); }, "add overflow not detected");
    require_overflow([&] { (void)checked_multiply(maximum, 2); }, "multiply overflow not detected");
    require_overflow([&] { (void)checked_add(maximum - 2, 3); },
                     "near maximum addition not detected");
}
} // namespace

int main() {
    try {
        test_frozen_coefficients_and_binary_strings();
        test_list_estimator();
        test_hash_estimator_order_independent();
        test_set_uniqueness();
        test_expiration_duplicate_key_charge();
        test_checked_arithmetic();
        std::cout << "Phase 6F.1B pure estimator policy tests PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
