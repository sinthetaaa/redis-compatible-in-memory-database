#include "redisdb/persistence/AofLog.hpp"
#include "redisdb/storage/Database.hpp"
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
long budget = -1;
bool count_bytes = false;
std::size_t requested_bytes = 0, trials = 0;
} // namespace
void* operator new(std::size_t n) {
    if (budget == 0)
        throw std::bad_alloc{};
    if (budget > 0)
        --budget;
    if (void* p = std::malloc(n ? n : 1)) {
        if (count_bytes)
            requested_bytes += n;
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
namespace {
using namespace redisdb::storage;
using namespace std::chrono_literals;
namespace persistence = redisdb::persistence;
void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
class ManualClock final : public Clock {
  public:
    WallTimePoint wall{1000s};
    WallTimePoint wall_now() const noexcept override { return wall; }
    MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
};
std::string state(const Database& db) {
    CommittedEffects result = db.snapshot_order();
    db.visit_snapshot(
        [&](const auto& key, const auto& entry) { result.entries.push_back({key, entry}); });
    return persistence::encode_effects(result) + ":" + std::to_string(db.accounted_bytes());
}
void seed(Database& db) {
    db.set_many({{"a", std::string(80, 'a')}, {"n", "9"}, {"untouched", "safe"}});
    (void)db.list_push("list", {"one", "two"}, false);
    (void)db.list_push("last", {"one"}, false);
    (void)db.hash_set("hash", {{"f", "v"}, {"g", "w"}});
    (void)db.set_add("set", {"x", "y"});
    (void)db.set_with_expiration("ttl", "old", 10000, true);
}
void allocation_campaign(const std::function<void(Database&)>& operation, EvictionPolicy policy) {
    bool succeeded = false;
    for (long fail_at = 0; fail_at < 1000; ++fail_at) {
        ++trials;
        ManualClock clock;
        Database db(clock, MemoryOptions{{}, policy});
        seed(db);
        const auto before = state(db);
        budget = fail_at;
        try {
            Database::Transaction tx(db);
            operation(db);
            (void)persistence::encode_effects(tx.effects());
            // Even successful staging/effect encoding must roll back with all
            // further allocations forbidden. Never commit this trial.
            budget = 0;
            succeeded = true;
        } catch (const std::bad_alloc&) {
        } catch (...) {
            budget = -1;
            throw;
        }
        budget = -1;
        check(state(db) == before,
              "allocation failure/forced rollback changed state, TTL, LRU or ledger");
        // Exercise index reconstruction after rollback, including backup expiry nodes.
        {
            Database::Transaction tx(db);
            (void)db.clear_expiration("ttl");
        }
        check(state(db) == before, "restored expiry index is invalid");
        if (succeeded)
            break;
    }
    check(succeeded, "allocation campaign never completed");
}
void undo_and_postimages() {
    for (auto policy : {EvictionPolicy::NoEviction, EvictionPolicy::AllKeysLru}) {
        const std::vector<std::function<void(Database&)>> operations = {
            [](auto& db) { (void)db.set("a", "new"); },
            [](auto& db) { (void)db.set(std::string("new\0key", 7), std::string("\0\xff", 2)); },
            [](auto& db) { db.set_many({{"a", "first"}, {"new", "v"}, {"a", "last"}}); },
            [](auto& db) { (void)db.append("a", "suffix", 1000); },
            [](auto& db) { (void)db.adjust_integer("n", 1, false); },
            [](auto& db) {
                (void)db.list_pop("list", true);
                (void)db.list_pop("last", false);
            },
            [](auto& db) { (void)db.list_push("list", {"x", "y"}, true); },
            [](auto& db) { (void)db.hash_set("hash", {{"f", "new"}, {"binary\0", "v"}}); },
            [](auto& db) { (void)db.hash_delete("hash", {"f", "g"}); },
            [](auto& db) {
                (void)db.set_add("set", {"new"});
                (void)db.set_remove("set", {"x", "y", "new"});
            },
            [](auto& db) { (void)db.clear_expiration("ttl"); },
            [](auto& db) { (void)db.set_with_expiration("ttl", "new", 100, true); },
            [](auto& db) { (void)db.expire_after("a", 0, true); },
            [](auto& db) {
                (void)db.erase("a");
                (void)db.set("a", "new");
                (void)db.erase("ttl");
            },
            [](auto& db) {
                db.clear();
                db.set_many({{"new", "one"}, {"second", "two"}});
            },
            [](auto& db) {
                (void)db.set("a", "changed");
                db.clear();
                (void)db.set("ttl", "new");
                db.clear();
                (void)db.set("a", "last");
            },
            [](auto& db) {
                std::vector<std::pair<std::string, std::string>> pairs;
                for (int i = 0; i < 100; ++i)
                    pairs.emplace_back("rehash:" + std::to_string(i), std::string(64, 'x'));
                db.set_many(pairs);
                (void)db.clear_expiration("ttl");
            }};
        for (const auto& operation : operations)
            allocation_campaign(operation, policy);
        ManualClock clock;
        Database db(clock, MemoryOptions{{}, policy});
        seed(db);
        const auto before = state(db);
        {
            Database::Transaction tx(db);
            (void)db.set("a", std::string(80, 'a'));
            check(tx.effects().entries.empty(), "no-op journaled");
            tx.commit();
        }
        // A successful LRU access may change recency without a data effect;
        // V1 captures that order at the next data commit, as before.
        if (policy == EvictionPolicy::NoEviction)
            check(state(db) == before, "no-op changed state");
        {
            Database::Transaction tx(db);
            (void)db.set("a", "new");
            (void)db.erase("n");
            (void)db.set("new", "v");
            const auto effects = tx.effects();
            check(effects.entries.size() == 3, "wrong committed postimage/tombstone count");
            check(effects.entries[0].key == "a" && effects.entries[1].key == "n" &&
                      !effects.entries[1].replacement && effects.entries[2].key == "new",
                  "effects not canonical");
            tx.commit();
        }
        check(db.get("a") == "new" && !db.exists("n") && db.get("new") == "v",
              "commit reply state incorrect");
    }
}
void expiration_capacity_and_scaling() {
    ManualClock clock;
    Database db(clock, MemoryOptions{200});
    (void)db.set_with_expiration("ttl", "v", 1, true);
    (void)db.set("live", "safe");
    clock.wall += 1ms;
    const auto before = state(db);
    {
        Database::Transaction tx(db);
        check(!db.get("ttl"), "lazy expiry");
        check(tx.effects().entries.size() == 1, "expiry tombstone missing");
        budget = 0;
    }
    budget = -1;
    check(state(db) == before, "lazy expiry rollback lost physical state");
    {
        Database::Transaction tx(db);
        (void)db.sweep_expired(32);
        budget = 0;
    }
    budget = -1;
    check(state(db) == before, "active expiry rollback lost state");
    {
        Database::Transaction tx(db);
        (void)db.set("replacement", std::string(40, 'r'));
        budget = 0;
    }
    budget = -1;
    check(state(db) == before, "expired capacity victim rollback lost state");
    try {
        Database::Transaction tx(db);
        db.set_many({{"live", "changed"}, {"too-large", std::string(1000, 'x')}});
        throw std::runtime_error("OOM accepted");
    } catch (const MemoryLimitError&) {
    }
    check(state(db) == before, "MSET OOM did not roll back");
    Database large;
    (void)large.set("untouched", std::string(1024 * 1024, 'z'));
    (void)large.set("target", "old");
    requested_bytes = 0;
    count_bytes = true;
    {
        Database::Transaction tx(large);
        (void)large.set("target", "new");
        check(tx.effects().entries.size() == 1, "single-key effect missing");
        tx.commit();
    }
    count_bytes = false;
    check(requested_bytes < 16384, "unrelated value copied by incremental transaction");
    {
        Database::Transaction tx(large);
        bool refused = false;
        try {
            Database::Transaction nested(large);
        } catch (const std::logic_error&) {
            refused = true;
        }
        check(refused, "nested transaction accepted");
    }
}

void committed_clear_postimages() {
    for (auto policy : {EvictionPolicy::NoEviction, EvictionPolicy::AllKeysLru}) {
        ManualClock clock;
        Database db(clock, MemoryOptions{{}, policy});
        seed(db);
        Database recovered(clock, MemoryOptions{{}, policy});
        db.visit_snapshot([&](const auto& key, const auto& entry) {
            recovered.restore_snapshot_entry({key, entry});
        });
        recovered.replay_effects(db.snapshot_order());
        const auto original_keys = db.size();
        CommittedEffects effects;
        {
            Database::Transaction tx(db);
            (void)db.set("a", "changed-before-clear");
            (void)db.set("temporary", "never-committed");
            budget = 0;
            db.clear(); // historical noexcept boundary must not allocate
            budget = -1;
            (void)db.set("ttl", "temporary-replacement");
            db.clear();
            (void)db.set("a", "final");
            effects = tx.effects();
            check(effects.entries.size() == original_keys, "clear effects lost original keys");
            for (const auto& effect : effects.entries)
                check(effect.key != "temporary", "clear journaled an uncommitted intermediate key");
            tx.commit();
            budget = 0; // committed destruction must also avoid allocation
        }
        budget = -1;
        recovered.replay_effects(effects);
        check(db.size() == 1 && db.get("a") == "final", "clear commit retained intermediate state");
        check(state(db) == state(recovered), "clear postimages/tombstones do not replay exactly");
    }
}
} // namespace
int main() {
    try {
        undo_and_postimages();
        expiration_capacity_and_scaling();
        committed_clear_postimages();
        std::cout << "PASS " << trials
                  << " allocation trials; incremental undo, postimages, no-allocation rollback, "
                     "TTL/capacity and LRU fallback\n";
    } catch (const std::exception& error) {
        budget = -1;
        count_bytes = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
