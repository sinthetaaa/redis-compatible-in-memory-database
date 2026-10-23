#include "redisdb/storage/Entry.hpp"
#include "redisdb/storage/Value.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

using redisdb::storage::Entry;
using redisdb::storage::HashValue;
using redisdb::storage::ListValue;
using redisdb::storage::SetValue;
using redisdb::storage::StringValue;
using redisdb::storage::Value;
using redisdb::storage::WallTimePoint;

namespace {

void check(bool result, const char* explanation) {
    if (!result) {
        throw std::runtime_error(explanation);
    }
}

void test_type_identity_and_safe_access() {
    static_assert(std::variant_size_v<Value> == 4);
    static_assert(std::is_copy_constructible_v<Value>);
    static_assert(std::is_copy_assignable_v<Value>);
    static_assert(std::is_nothrow_move_constructible_v<Value>);
    static_assert(std::is_nothrow_move_assignable_v<Value>);
    static_assert(std::is_same_v<std::variant_alternative_t<0, Value>, StringValue>);
    static_assert(std::is_same_v<std::variant_alternative_t<1, Value>, ListValue>);
    static_assert(std::is_same_v<std::variant_alternative_t<2, Value>, HashValue>);
    static_assert(std::is_same_v<std::variant_alternative_t<3, Value>, SetValue>);

    const Value initial;
    check(std::holds_alternative<StringValue>(initial), "default type must remain string");
    check(std::get<StringValue>(initial).bytes.empty(), "default stored string is not empty");
    check(std::get_if<HashValue>(&initial) == nullptr, "wrong variant alternative was accessed");

    const Value values[] = {StringValue{}, ListValue{}, HashValue{}, SetValue{}};
    check(std::holds_alternative<StringValue>(values[0]), "string variant tag");
    check(std::holds_alternative<ListValue>(values[1]), "list variant tag");
    check(std::holds_alternative<HashValue>(values[2]), "hash variant tag");
    check(std::holds_alternative<SetValue>(values[3]), "set variant tag");
    check(values[0] != values[1] && values[1] != values[2] && values[2] != values[3],
          "different empty types must not be equal");
}

void test_string_ownership_and_binary_bytes() {
    std::string input("A\0\x80\xff", 4);
    Value original = StringValue{input};
    Value copy = original;
    input[0] = 'Z';
    check(std::get<StringValue>(original).bytes == std::string("A\0\x80\xff", 4),
          "value aliases source buffer");
    std::get<StringValue>(copy).bytes[1] = 'Q';
    check(copy != original, "string copy did not own independent bytes");
    check(std::get<StringValue>(original).bytes[1] == '\0', "string copy mutated original");
    Value moved = std::move(copy);
    check(std::get<StringValue>(moved).bytes == std::string("AQ\x80\xff", 4),
          "string move lost binary bytes");
}

void test_list_order_duplicates_and_snapshot() {
    const std::string binary("L\0\xff", 3);
    ListValue list;
    check(list.elements.empty(), "new list was not empty");
    list.elements.push_back("");
    list.elements.push_back(binary);
    list.elements.push_front(binary);
    list.elements.push_back(binary);
    Value original{std::move(list)};
    check(std::holds_alternative<ListValue>(original), "list type changed");
    const auto& sequence = std::get<ListValue>(original).elements;
    check(sequence.size() == 4, "list incorrectly removed duplicates");
    auto it = sequence.begin();
    check(*it++ == binary && *it++ == "" && *it++ == binary && *it++ == binary,
          "list order or binary bytes changed");

    Value copy = original;
    auto& copy_list = std::get<ListValue>(copy).elements;
    copy_list.front() = "changed";
    copy_list.pop_back();
    check(std::get<ListValue>(original).elements.size() == 4 &&
              std::get<ListValue>(original).elements.front() == binary,
          "list snapshot mutated original");
    Value moved = std::move(copy);
    check(std::get<ListValue>(moved).elements.size() == 3, "list move lost elements");
}

void test_hash_field_ownership_and_overwrite() {
    const std::string binary_field("f\0\xff", 3);
    const std::string binary_value("\0\x80\xff", 3);
    HashValue hash;
    check(hash.fields.empty(), "new hash was not empty");
    hash.fields.insert_or_assign(binary_field, binary_value);
    hash.fields.insert_or_assign("", "");
    check(!hash.fields.insert_or_assign(binary_field, "last").second,
          "duplicate field did not overwrite");
    check(hash.fields.size() == 2 && hash.fields.at(binary_field) == "last",
          "hash overwrite or empty field failed");
    hash.fields.insert_or_assign(binary_field, binary_value);
    Value original{std::move(hash)};
    Value copy = original;
    std::get<HashValue>(copy).fields.at(binary_field) = "different";
    std::get<HashValue>(copy).fields.insert_or_assign("added", "x");
    check(std::get<HashValue>(original).fields.at(binary_field) == binary_value,
          "hash copy aliased binary payload");
    check(std::get<HashValue>(original).fields.size() == 2, "hash copy aliased field index");
    check(std::get<HashValue>(original).fields.at("").empty(), "empty hash field/value lost");
}

void test_set_uniqueness_and_copy() {
    const std::string binary("\0\x80\xff", 3);
    SetValue set;
    check(set.members.empty(), "new set was not empty");
    check(set.members.insert(binary).second, "first binary insertion failed");
    check(!set.members.insert(binary).second, "duplicate member not suppressed");
    check(set.members.insert("").second, "empty member not accepted");
    check(set.members.insert("x").second, "ordinary member not accepted");
    Value original{std::move(set)};
    Value copy = original;
    std::get<SetValue>(copy).members.erase(binary);
    std::get<SetValue>(copy).members.insert("extra");
    const auto& members = std::get<SetValue>(original).members;
    check(members.size() == 3 && members.contains(binary) && members.contains("") &&
              !members.contains("extra"),
          "set copy shared membership state");
}

void test_entry_compatibility_and_ttl_ownership() {
    const WallTimePoint deadline{std::chrono::seconds{123}};
    const Entry old_initializer{Value{StringValue{"old"}}};
    check(!old_initializer.expires_at.has_value(), "old aggregate initializer changed");
    check(std::get<StringValue>(old_initializer.value).bytes == "old",
          "old string aggregate initialization failed");

    Entry owned{Value{ListValue{}}, deadline};
    std::get<ListValue>(owned.value).elements.push_back(std::string("\0\xff", 2));
    Entry copy = owned;
    std::get<ListValue>(copy.value).elements.push_back("new");
    copy.expires_at.reset();
    check(owned.expires_at == deadline && copy.expires_at == std::nullopt,
          "entry copied expiration metadata incorrectly");
    check(std::get<ListValue>(owned.value).elements.size() == 1 &&
              std::get<ListValue>(copy.value).elements.size() == 2,
          "entry copy aliased collection");

    Entry moved = std::move(owned);
    check(moved.expires_at == deadline &&
              std::get<ListValue>(moved.value).elements.front() == std::string("\0\xff", 2),
          "entry move lost deadline or binary bytes");

    Entry empty_hash{Value{HashValue{}}};
    Entry empty_set{Value{SetValue{}}};
    check(!empty_hash.expires_at && !empty_set.expires_at,
          "default expiration should be absent for all typed entries");
}

} // namespace

int main() {
    try {
        test_type_identity_and_safe_access();
        test_string_ownership_and_binary_bytes();
        test_list_order_duplicates_and_snapshot();
        test_hash_field_ownership_and_overwrite();
        test_set_uniqueness_and_copy();
        test_entry_compatibility_and_ttl_ownership();
        std::cout << "PASS: owned typed storage values and Entry compatibility\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
