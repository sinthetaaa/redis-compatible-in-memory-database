#include "redisdb/storage/Database.hpp"

#include <cstddef>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using redisdb::storage::Database;

namespace {
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

void test_basic_semantics() {
    Database db;
    check(db.size() == 0 && !db.get("missing").has_value(), "empty database");
    check(!db.exists("missing") && !db.erase("missing"), "missing key semantics");

    check(db.set("one", "alpha"), "first insert must create key");
    check(!db.set("one", "beta"), "replace must not create key");
    check(db.size() == 1 && db.get("one") == "beta", "replace must be atomic");
    check(db.set("ONE", "uppercase"), "key lookup must be case sensitive");
    check(db.size() == 2 && db.exists("ONE"), "case-sensitive key count");

    check(db.set("", "empty-key"), "empty key must be allowed");
    check(db.get("") == "empty-key", "empty key lookup");
    check(!db.set("one", ""), "empty value replacement");
    check(db.get("one") == "", "empty string must be present, not absent");

    const auto snapshot = db.get("ONE");
    check(snapshot == "uppercase", "snapshot initial value");
    check(!db.set("ONE", "changed"), "snapshot overwrite");
    check(snapshot == "uppercase" && db.get("ONE") == "changed", "get must return owned snapshot");
    check(db.erase("ONE") && !db.erase("ONE"), "erase idempotence");
    check(!db.exists("ONE") && db.size() == 2, "erase count");
    db.clear();
    check(db.size() == 0 && !db.exists("one"), "clear must remove all entries");
}

void test_binary_safety_and_isolation() {
    Database first;
    Database second;
    const std::string key("ab\0cd\r\n", 7);
    const std::string value("V\0\r\n\x80\xff", 6);
    check(first.set(key, value), "binary set");
    check(first.exists(std::string_view(key.data(), key.size())), "binary lookup");
    check(first.get(key) == value, "binary value round trip");
    check(!second.get(key).has_value(), "databases must be isolated");
    check(first.set("ab", "other"), "embedded NUL must not truncate keys");
    check(first.get("ab") == "other" && first.get(key) == value,
          "binary keys must not collide with prefixes");
    std::string large(32 * 1024, 'x');
    large.front() = '\0';
    large.back() = '\n';
    check(first.set("large", large) && first.get("large") == large, "large payload round trip");
    check(first.erase(key) && !first.exists(key), "binary erase");
    check(!second.erase(key) && second.size() == 0, "independent keyspaces");
}

void test_deterministic_reference_model() {
    Database db;
    std::unordered_map<std::string, std::string> reference;
    std::minstd_rand rng(20261023);
    std::vector<std::string> keys;
    for (int i = 0; i < 128; ++i) {
        std::string key = "key-" + std::to_string(i);
        if (i % 7 == 0) {
            key.push_back('\0');
            key.push_back(static_cast<char>(i));
        }
        keys.push_back(std::move(key));
    }
    for (int iteration = 0; iteration < 20000; ++iteration) {
        const auto index = static_cast<std::size_t>(rng() % keys.size());
        const std::string& key = keys[index];
        const auto operation = rng() % 5;
        if (operation <= 1) {
            std::string value(static_cast<std::size_t>(rng() % 127),
                              static_cast<char>(rng() % 256));
            if (operation == 1) {
                value += std::to_string(iteration);
            }
            const bool inserted = db.set(key, value);
            const bool expected = reference.insert_or_assign(key, value).second;
            check(inserted == expected, "insertion/replacement discrepancy");
        } else if (operation == 2) {
            const bool removed = db.erase(key);
            check(removed == (reference.erase(key) == 1), "erase discrepancy");
        } else if (operation == 3) {
            const auto it = reference.find(key);
            const std::optional<std::string> expected =
                it == reference.end() ? std::nullopt : std::optional<std::string>{it->second};
            check(db.get(key) == expected, "get discrepancy");
        } else {
            check(db.exists(key) == reference.contains(key), "exists discrepancy");
        }
        check(db.size() == reference.size(), "size discrepancy");
    }
    for (const auto& key : keys) {
        const auto it = reference.find(key);
        const std::optional<std::string> expected =
            it == reference.end() ? std::nullopt : std::optional<std::string>{it->second};
        check(db.get(key) == expected, "final reference mismatch");
    }
}
} // namespace

int main() {
    try {
        test_basic_semantics();
        test_binary_safety_and_isolation();
        test_deterministic_reference_model();
        std::cout << "PASS: Phase 4A keyspace, binary safety, 20000 reference checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
