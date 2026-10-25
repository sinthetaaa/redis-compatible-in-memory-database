#include "redisdb/storage/Database.hpp"
#include "redisdb/storage/MemoryAccounting.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

// Exhaust every allocation point in real storage operations, including node,
// index, rehash and victim-plan allocation. No production failure hooks.
namespace {
long allocations_before_failure = -1;
}
void* operator new(std::size_t size) {
    if (allocations_before_failure == 0) {
        throw std::bad_alloc{};
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace redisdb::storage;
using namespace std::chrono_literals;

void check(bool value, const char* why) {
    if (!value) {
        throw std::runtime_error(why);
    }
}
template <class Fn> void oom(Fn fn) {
    try {
        fn();
    } catch (const MemoryLimitError&) {
        return;
    }
    throw std::runtime_error("expected capacity rejection");
}
class ManualClock final : public Clock {
  public:
    WallTimePoint now{1000s};
    WallTimePoint wall_now() const noexcept override { return now; }
    MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
};

void boundaries_and_replacement() {
    Database db(MemoryOptions{66});
    check(db.set("a", "x"), "exact boundary");
    check(db.accounted_bytes() == 66, "boundary ledger");
    oom([&] { (void)db.append("a", "y", 100); });
    check(db.get("a") == "x" && db.accounted_bytes() == 66, "append atomic");
    oom([&] { (void)db.set("b", ""); });
    check(db.size() == 1 && !db.exists("b"), "failed insert absent");
    (void)db.set("a", "");
    check(db.accounted_bytes() == 65, "shrink allowed");
    (void)db.set("a", "y");
    check(db.erase("a") && db.accounted_bytes() == 0, "delete frees capacity");
    Database zero(MemoryOptions{0});
    oom([&] { (void)zero.set("", ""); });
    Database unlimited;
    (void)unlimited.set("big", std::string(20000, 'x'));
    check(unlimited.accounted_bytes() == 20067, "default unlimited");
}

void mutation_paths() {
    Database list(MemoryOptions{114});
    (void)list.list_push("l", {"x"}, false);
    oom([&] { (void)list.list_push("l", {"y"}, true); });
    check(list.list_length("l") == 1 && list.accounted_bytes() == 114, "list rollback");
    check(list.list_pop("l", true) == "x" && list.accounted_bytes() == 0, "list removal ledger");
    Database hash(MemoryOptions{147});
    (void)hash.hash_set("h", {{"f", "v"}});
    oom([&] { (void)hash.hash_set("h", {{"f", "long"}, {"g", "z"}}); });
    check(hash.hash_get("h", "f") == "v" && hash.hash_length("h") == 1 &&
              hash.accounted_bytes() == 147,
          "hash rollback");
    (void)hash.hash_set("h", {{"f", "q"}, {"f", "r"}});
    check(hash.hash_get("h", "f") == "r", "hash replacement at limit");
    Database set(MemoryOptions{122});
    (void)set.set_add("s", {"m", "m"});
    oom([&] { (void)set.set_add("s", {"n", "m"}); });
    check(set.set_cardinality("s") == 1 && set.accounted_bytes() == 122, "set rollback");
    check(set.set_add("s", {"m"}) == 0, "set noop at limit");
    Database number(MemoryOptions{66});
    (void)number.set("n", "9");
    oom([&] { (void)number.adjust_integer("n", 1, false); });
    check(number.get("n") == "9" && number.accounted_bytes() == 66, "integer growth rollback");
    check(number.adjust_integer("n", 1, true).value == 8, "integer same size allowed");
}

void mset_atomicity() {
    for (auto policy : {EvictionPolicy::NoEviction, EvictionPolicy::AllKeysLru}) {
        Database db(MemoryOptions{132, policy});
        db.set_many({{"a", "x"}, {"b", "y"}});
        oom([&] { db.set_many({{"a", "xx"}, {"b", "yy"}}); });
        check(db.get("a") == "x" && db.get("b") == "y" && db.accounted_bytes() == 132,
              "MSET destinations protected, atomic rejection");
        db.set_many({{"a", "xx"}, {"b", ""}, {"a", "z"}});
        check(db.get("a") == "z" && db.get("b") == "" && db.accounted_bytes() == 131,
              "MSET final duplicate footprint");
    }
    Database db(MemoryOptions{132, EvictionPolicy::AllKeysLru});
    db.set_many({{"b", "x"}, {"a", "x"}});
    (void)db.set("c", "x");
    check(!db.exists("a") && db.exists("b") && db.exists("c"), "MSET binary-key tie order");
}

void lru_victims() {
    Database db(MemoryOptions{132, EvictionPolicy::AllKeysLru});
    (void)db.set("a", "x");
    (void)db.set("b", "y");
    (void)db.get("a");
    (void)db.set("c", "z");
    check(!db.exists("b") && db.get("a") == "x" && db.get("c") == "z", "read refreshes LRU");
    oom([&] { (void)db.set("too-big", std::string(200, 'x')); });
    check(db.size() == 2 && db.accounted_bytes() == 132, "impossible write evicts nothing");
    (void)db.set("a", "xx");
    check(db.size() == 1 && db.get("a") == "xx" && db.accounted_bytes() == 67,
          "growing target cannot evict itself");
    db.clear();
    (void)db.set("d", "x");
    (void)db.set("e", "y");
    (void)db.set("f", "z");
    check(!db.exists("d") && db.accounted_bytes() == 132, "clear resets recency index");

    // A larger incoming write must reclaim multiple victims, not just one.
    Database many(MemoryOptions{198, EvictionPolicy::AllKeysLru});
    many.set_many({{"a", "x"}, {"b", "x"}, {"c", "x"}});
    (void)many.set("z", std::string(100, 'v'));
    check(many.size() == 1 && many.accounted_bytes() == 165, "multiple LRU victims");
}

template <class Setup, class Read>
void collection_recency(std::size_t charge, Setup setup, Read read) {
    Database db(MemoryOptions{charge + 66, EvictionPolicy::AllKeysLru});
    setup(db);
    (void)db.set("b", "x");
    read(db);
    (void)db.set("c", "y");
    check(db.exists("a") && !db.exists("b") && db.exists("c"), "collection read refreshes LRU");
    check(db.accounted_bytes() == charge + 66, "collection eviction ledger");
}

void all_type_recency() {
    collection_recency(
        114, [](Database& db) { (void)db.list_push("a", {"x"}, false); },
        [](Database& db) { (void)db.list_range("a", 0, -1, 4096); });
    collection_recency(
        147, [](Database& db) { (void)db.hash_set("a", {{"f", "v"}}); },
        [](Database& db) { (void)db.hash_items("a", 4096); });
    collection_recency(
        122, [](Database& db) { (void)db.set_add("a", {"m"}); },
        [](Database& db) { (void)db.set_members("a", 4096); });
    collection_recency(
        66, [](Database& db) { (void)db.set("a", "x"); },
        [](Database& db) { (void)db.remaining_ttl("a", true); });
}

void ttl_capacity() {
    ManualClock clock;
    Database db(clock, MemoryOptions{66});
    (void)db.set("a", "x");
    oom([&] { (void)db.expire_after("a", 10, false); });
    oom([&] { (void)db.set_expiration("a", clock.now + 10s); });
    oom([&] { (void)db.set_with_expiration("a", "y", 10, false); });
    check(db.get("a") == "x" && db.remaining_ttl("a", true) == -1 && db.accounted_bytes() == 66,
          "TTL capacity rejection preserves value and metadata");
    check(db.set_with_expiration("a", "z", 0, false) == SetWithExpiryResult::OutOfRange,
          "invalid SET TTL precedes capacity");
    check(db.expire_after("a", 0, false) == ExpireResult::Deleted && db.accounted_bytes() == 0,
          "immediate expiry frees space at capacity");

    for (auto policy : {EvictionPolicy::NoEviction, EvictionPolicy::AllKeysLru}) {
        Database timed(clock, MemoryOptions{181, policy});
        (void)timed.set("a", "x");                           // oldest live key
        (void)timed.set_with_expiration("b", "y", 10, true); // 115 bytes
        clock.now += 10ms;
        (void)timed.set("c", "z");
        check(timed.get("a") == "x" && !timed.exists("b") && timed.get("c") == "z" &&
                  timed.accounted_bytes() == 132,
              "expired reclaimed before oldest live key");
    }
    Database timed(clock, MemoryOptions{181, EvictionPolicy::AllKeysLru});
    (void)timed.set_with_expiration("a", "x", 10, false);
    (void)timed.set("b", "y");
    oom([&] { timed.set_many({{"a", std::string(100, 'z')}, {"b", "q"}}); });
    check(timed.get("a") == "x" && timed.remaining_ttl("a", true) == 10000 &&
              timed.get("b") == "y" && timed.accounted_bytes() == 181,
          "rejected MSET preserves target TTLs");
    check(timed.clear_expiration("a") && timed.accounted_bytes() == 132, "PERSIST frees metadata");
    (void)timed.expire_after("a", 1, true);
    clock.now += 1ms;
    check(timed.sweep_expired(10).deleted == 1, "active expiry removes recency node");
    (void)timed.set("c", "z");
    (void)timed.set("d", "v");
    check(!timed.exists("b") && timed.accounted_bytes() == 132, "LRU works after active expiry");
}

void validation_never_evicts() {
    Database db(MemoryOptions{133, EvictionPolicy::AllKeysLru});
    (void)db.set("a", "bad");
    (void)db.set("b", ""); // 133 total
    check(db.adjust_integer("a", 1, false).error == IntegerMutationError::InvalidInteger,
          "invalid integer precedence");
    check(!db.append("a", "too long", 3), "value limit precedence");
    try {
        (void)db.list_push("a", {"x"}, true);
        throw std::runtime_error("wrong type accepted");
    } catch (const WrongTypeError&) {
    }
    check(db.get("a") == "bad" && db.exists("b") && db.accounted_bytes() == 133,
          "validation failure evicts nothing");
}

template <class Fn> void fail_allocations(Fn operation) {
    bool succeeded = false;
    for (long budget = 0; budget < 500; ++budget) {
        ManualClock clock;
        Database db(clock, MemoryOptions{370, EvictionPolicy::AllKeysLru});
        const std::string victim_key(40, 'a'); // expiry-index erasure must not copy/allocate
        (void)db.set_with_expiration(victim_key, "x", 100, false);
        (void)db.set("b", std::string(80, 'b'));
        const auto before = db.accounted_bytes(); // 338 bytes
        allocations_before_failure = budget;
        try {
            operation(db);
            allocations_before_failure = -1;
            check(db.accounted_bytes() <= 370, "successful allocation trial exceeded limit");
            succeeded = true;
            break;
        } catch (const std::bad_alloc&) {
            allocations_before_failure = -1;
            check(db.size() == 2 && db.accounted_bytes() == before && db.get(victim_key) == "x" &&
                      db.get("b") == std::string(80, 'b') &&
                      db.remaining_ttl(victim_key, true) == 100000,
                  "allocation failure modified data, evicted key or changed TTL/ledger");
            // Existing expiry and recency indexes must remain usable.
            clock.now += 100s;
            check(db.sweep_expired(10).deleted == 1 && db.accounted_bytes() == 145,
                  "index corrupted by failed allocation");
        } catch (...) {
            allocations_before_failure = -1;
            throw;
        }
    }
    check(succeeded, "allocation failure campaign never reached success");
}

void allocation_safety() {
    fail_allocations([](Database& db) { (void)db.set("c", std::string(90, 'c')); });
    fail_allocations([](Database& db) { (void)db.append("b", std::string(60, 'c'), 1000); });
    fail_allocations([](Database& db) {
        db.set_many({{std::string(40, 'a'), std::string(100, 'q')}, {"c", "z"}});
    });
    fail_allocations([](Database& db) { (void)db.list_push("c", {"one", "two"}, false); });
    fail_allocations([](Database& db) { (void)db.hash_set("c", {{"f", "value"}}); });
    fail_allocations([](Database& db) { (void)db.set_add("c", {"one", "two"}); });
    fail_allocations([](Database& db) { (void)db.set_with_expiration("c", "z", 10, false); });
    fail_allocations([](Database& db) { (void)db.expire_after("b", 10, false); });
}
} // namespace

int main() {
    try {
        boundaries_and_replacement();
        mutation_paths();
        mset_atomicity();
        lru_victims();
        all_type_recency();
        ttl_capacity();
        validation_never_evicts();
        allocation_safety();
        std::cout << "memory limits: boundaries, all mutations, LRU, TTL and allocation rollback "
                     "passed\n";
    } catch (const std::exception& error) {
        allocations_before_failure = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
