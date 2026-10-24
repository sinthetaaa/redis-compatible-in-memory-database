#include "redisdb/storage/Clock.hpp"
#include "redisdb/storage/Database.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using redisdb::storage::Clock;
using redisdb::storage::Database;
using redisdb::storage::ExpireResult;
using redisdb::storage::IntegerMutationError;
using redisdb::storage::MonotonicTimePoint;
using redisdb::storage::SetWithExpiryResult;
using redisdb::storage::WallTimePoint;
using redisdb::storage::WrongTypeError;

static_assert(noexcept(std::declval<const Database&>().accounted_bytes()));
static_assert(noexcept(std::declval<Database&>().clear()));

void check(bool valid, const char* why) {
    if (!valid) {
        throw std::runtime_error(why);
    }
}

class ManualClock final : public Clock {
  public:
    [[nodiscard]] MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override {
        ++reads_;
        return now_;
    }
    void advance(std::chrono::milliseconds amount) { now_ += amount; }
    [[nodiscard]] std::size_t reads() const noexcept { return reads_; }

  private:
    WallTimePoint now_{WallTimePoint{} + 1000s};
    mutable std::size_t reads_ = 0;
};

// Oracle constants and formulas are independent of the production estimator.
constexpr std::size_t key_cost(std::string_view key) { return 64 + key.size(); }
constexpr std::size_t expiry_cost(std::string_view key) { return 48 + key.size(); }
constexpr std::size_t str_cost(std::string_view key, std::string_view bytes, bool timed = false) {
    return key_cost(key) + bytes.size() + (timed ? expiry_cost(key) : 0);
}
constexpr std::size_t list_cost(std::string_view key, std::size_t count, std::size_t payload,
                                bool timed = false) {
    return key_cost(key) + count * 48 + payload + (timed ? expiry_cost(key) : 0);
}
constexpr std::size_t hash_cost(std::string_view key, std::size_t fields, std::size_t payload,
                                bool timed = false) {
    return key_cost(key) + fields * 80 + payload + (timed ? expiry_cost(key) : 0);
}
constexpr std::size_t set_cost(std::string_view key, std::size_t members, std::size_t payload,
                               bool timed = false) {
    return key_cost(key) + members * 56 + payload + (timed ? expiry_cost(key) : 0);
}
void total(const Database& db, std::size_t expected, const char* where) {
    if (db.accounted_bytes() != expected) {
        std::cerr << "ledger mismatch at " << where << ": actual=" << db.accounted_bytes()
                  << " expected=" << expected << '\n';
        throw std::runtime_error(where);
    }
}
void test_string_and_ttl_paths() {
    ManualClock clock;
    Database db(clock);
    total(db, 0, "empty");
    const auto reads_before = clock.reads();
    (void)db.accounted_bytes();
    check(clock.reads() == reads_before, "accessor touched clock");
    check(db.set("", ""), "empty key must insert");
    total(db, str_cost("", ""), "empty binary-safe string");
    const std::string key("k\0", 2);
    const std::string value("A\0\xff", 3);
    check(db.set(key, value), "new binary key");
    auto wanted = str_cost("", "") + str_cost(key, value);
    total(db, wanted, "binary strings");
    check(!db.set(key, "ab"), "string replace");
    wanted = str_cost("", "") + str_cost(key, "ab");
    total(db, wanted, "string shrink");
    check(db.set_expiration(key, clock.wall_now() + 100ms), "attach ttl");
    wanted += expiry_cost(key);
    total(db, wanted, "attach ttl");
    check(db.set_expiration(key, clock.wall_now() + 200ms), "replace ttl");
    total(db, wanted, "ttl replacement same cost");
    check(db.clear_expiration(key), "remove ttl");
    wanted -= expiry_cost(key);
    total(db, wanted, "ttl cleared");
    check(!db.clear_expiration(key), "repeated PERSIST no-op");
    total(db, wanted, "persist noop");
    auto r = db.append(key, "XYZ", 100);
    check(r == 5, "append bytes");
    wanted = str_cost("", "") + str_cost(key, "abXYZ");
    total(db, wanted, "append");
    check(!db.append(key, "oops", 5).has_value(), "append limit");
    total(db, wanted, "append rejected");
    check(db.adjust_integer("num", 12, false).error == IntegerMutationError::None,
          "missing integer initialized");
    wanted += str_cost("num", "12");
    total(db, wanted, "integer create");
    check(db.adjust_integer("num", 2, true).value == 10, "integer subtraction");
    total(db, wanted, "integer same length");
    check(db.set_with_expiration("num", "b", 50, true) == SetWithExpiryResult::Set,
          "set with expiration");
    wanted = wanted - str_cost("num", "10") + str_cost("num", "b", true);
    total(db, wanted, "SET PX adds expiry and resets old payload");
    check(db.set_with_expiration("num", "BAD", std::numeric_limits<std::int64_t>::max(), false) ==
              SetWithExpiryResult::OutOfRange,
          "reject huge deadline");
    total(db, wanted, "invalid deadline rollback");
    clock.advance(50ms);
    total(db, wanted, "expired but still physically stored");
    check(db.remaining_ttl("num", true) == -2, "TTL lazy deletion");
    wanted -= str_cost("num", "b", true);
    total(db, wanted, "TTL physical deletion");
    check(db.erase(key), "erase binary key");
    wanted -= str_cost(key, "abXYZ");
    total(db, wanted, "erase binary key charged");
    db.clear();
    total(db, 0, "clear zero");
}

void test_collections() {
    ManualClock clock;
    Database db(clock);
    const std::string k("L\0", 2);
    check(db.list_push(k, {"a", "bc"}, false) == 2, "list push two");
    total(db, list_cost(k, 2, 3), "list insert");
    check(db.set_expiration(k, clock.wall_now() + 1000ms), "list expiry");
    total(db, list_cost(k, 2, 3, true), "list expiry");
    check(db.list_push(k, {"!"}, true) == 3, "left push");
    total(db, list_cost(k, 3, 4, true), "list grows with ttl");
    check(db.list_pop(k, true) == "!", "left pop");
    total(db, list_cost(k, 2, 3, true), "list first pop");
    check(db.list_pop(k, false) == "bc", "right pop");
    total(db, list_cost(k, 1, 1, true), "list second pop");
    check(db.list_pop(k, true) == "a", "last pop");
    total(db, 0, "list final erase");
    check(db.hash_set("h", {{"a", "x"}, {"b", "yz"}}) == 2, "hash insert");
    total(db, hash_cost("h", 2, 1 + 1 + 1 + 2), "hash initial fields");
    check(db.hash_set("h", {{"a", "mnop"}}) == 0, "hash overwrite");
    total(db, hash_cost("h", 2, 1 + 4 + 1 + 2), "hash replacement grows");
    check(db.set_expiration("h", clock.wall_now() + 1000ms), "hash ttl");
    total(db, hash_cost("h", 2, 1 + 4 + 1 + 2, true), "hash TTL");
    check(db.hash_delete("h", {"a"}) == 1, "hash partial delete");
    total(db, hash_cost("h", 1, 1 + 2, true), "hash partial shrink");
    check(db.hash_delete("h", {"b"}) == 1, "hash final erase");
    total(db, 0, "hash last field erased");
    const std::string binary("x\0", 2);
    check(db.set_add("s", {"one", "one", binary}) == 2, "set uniqueness");
    total(db, set_cost("s", 2, 3 + 2), "set added unique members");
    check(db.set_add("s", {"one"}) == 0, "set no-op");
    total(db, set_cost("s", 2, 5), "set no-op unchanged");
    check(db.set_remove("s", {binary}) == 1, "remove binary member");
    total(db, set_cost("s", 1, 3), "set remove one");
    check(db.set_remove("s", {"one"}) == 1, "remove final member");
    total(db, 0, "set final member erase");
}

void test_mset_expiry_sweeps_and_failures() {
    ManualClock clock;
    Database db(clock);
    check(db.set_with_expiration("x", "before", 20, true) == SetWithExpiryResult::Set,
          "timed seed x");
    check(db.set_with_expiration("z", "old", 100, true) == SetWithExpiryResult::Set,
          "timed seed z");
    auto want = str_cost("x", "before", true) + str_cost("z", "old", true);
    total(db, want, "two timed entries");
    clock.advance(20ms);
    total(db, want, "expired physical entry charged until removed");
    db.set_many({{"x", "new"}, {"x", "winner"}, {"y", "zero"}});
    want = str_cost("x", "winner") + str_cost("z", "old", true) + str_cost("y", "zero");
    total(db, want, "MSET duplicate last wins and expires old TTL");
    check(db.get("x") == "winner", "MSET last-wins value");
    auto sweep = db.sweep_expired(5);
    check(sweep.deleted == 0, "MSET old timed x should not expire");
    total(db, want, "sweep did not alter live keys");
    check(db.expire_after("y", 1, true) == ExpireResult::Set, "y expiry");
    want += expiry_cost("y");
    total(db, want, "relative expiry charged");
    check(db.expire_after("y", 1, true) == ExpireResult::Set, "y new deadline");
    total(db, want, "repeated TTL still one index cost");
    check(db.expire_after("y", 0, true) == ExpireResult::Deleted, "immediate expire deletes");
    want -= str_cost("y", "zero", true);
    total(db, want, "immediate expiry debit");
    check(db.set_with_expiration("bad", "stays", 10, true) == SetWithExpiryResult::Set,
          "new timed bad");
    want += str_cost("bad", "stays", true);
    total(db, want, "new timed bad");
    clock.advance(10ms);
    check(db.get("bad") == std::nullopt, "lazy get removes expired entry");
    want -= str_cost("bad", "stays", true);
    total(db, want, "lazy read removal");
    check(db.list_push("collection", {"one"}, true) == 1, "list seeded");
    want += list_cost("collection", 1, 3);
    total(db, want, "mixed list added");
    check(!db.set("collection", "single"), "cross-type plain SET");
    want = want - list_cost("collection", 1, 3) + str_cost("collection", "single");
    total(db, want, "cross-type replacement");
    bool wrongtype = false;
    try {
        (void)db.set_add("collection", {"err"});
    } catch (const WrongTypeError&) {
        wrongtype = true;
    }
    check(wrongtype, "WRONGTYPE exception expected");
    total(db, want, "wrong-type strong rollback");
    clock.advance(70ms);
    sweep = db.sweep_expired(1);
    check(sweep.deleted == 1, "bounded sweep deleted old z");
    want -= str_cost("z", "old", true);
    total(db, want, "sweep debits exactly one key");
    db.clear();
    total(db, 0, "clear resets all cached charges");
}
} // namespace

int main() {
    try {
        test_string_and_ttl_paths();
        test_collections();
        test_mset_expiry_sweeps_and_failures();
        std::cout << "PASS: independent Phase 6F.2C.1 live ledger scenarios\n";
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
