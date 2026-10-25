#include "redisdb/persistence/AofLog.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace redisdb;
using namespace std::chrono_literals;
using persistence::AofLog;
using persistence::AofOptions;
using persistence::SyncPolicy;
using storage::Database;

void check(bool value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
template <class Fn> void fails(Fn fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("expected failure");
}
struct Clock final : storage::Clock {
    storage::WallTimePoint wall{1000s};
    storage::MonotonicTimePoint mono{};
    storage::WallTimePoint wall_now() const noexcept override { return wall; }
    storage::MonotonicTimePoint monotonic_now() const noexcept override { return mono; }
};
struct Directory {
    std::filesystem::path path;
    Directory() {
        std::string name =
            (std::filesystem::temp_directory_path() / "redisdb-aof-unit-XXXXXX").string();
        if (!::mkdtemp(name.data()))
            throw std::runtime_error("mkdtemp");
        path = name;
    }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    AofOptions options(SyncPolicy policy = SyncPolicy::Always) const {
        return {true, path / "db.aof", policy};
    }
};
std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(bool(out), "write fixture");
}
template <class Fn> void apply(Database& db, AofLog& log, Fn fn) {
    Database::Transaction transaction(db);
    fn();
    log.append(transaction.effects());
    transaction.commit();
}

void roundtrip_and_cycles() {
    Directory directory;
    Clock clock;
    const std::string key("k\0\xff\r\n", 5), value("v\0\xff\r\n", 5);
    std::size_t total = 0;
    {
        Database db(clock);
        AofLog log(directory.options(), clock);
        log.recover(db);
        apply(db, log, [&] { (void)db.set(key, value); });
        apply(db, log, [&] { (void)db.list_push("l", {"a", "", value}, false); });
        apply(db, log, [&] { (void)db.hash_set("h", {{key, value}, {"", ""}}); });
        apply(db, log, [&] { (void)db.set_add("s", {key, "", key}); });
        apply(db, log, [&] { (void)db.adjust_integer("n", 9, false); });
        apply(db, log, [&] { (void)db.adjust_integer("n", 1, false); });
        apply(db, log, [&] { db.set_many({{"x", "first"}, {"y", "second"}, {"x", "last"}}); });
        apply(db, log, [&] { (void)db.set_with_expiration("ttl", "v", 5000, true); });
        apply(db, log, [&] { (void)db.set_with_expiration("gone", "v", 1, true); });
        total = db.accounted_bytes();
        log.finish();
    }
    for (int cycle = 0; cycle < 3; ++cycle) {
        Database recovered(clock);
        AofLog log(directory.options(), clock);
        log.recover(recovered);
        check(recovered.get(key) == value, "binary string recovery");
        check(recovered.list_range("l", 0, -1, 4096) == std::vector<std::string>({"a", "", value}),
              "list order/binary recovery");
        check(recovered.hash_get("h", key) == value && recovered.hash_get("h", "") == "",
              "hash recovery");
        check(recovered.set_cardinality("s") == 2 && recovered.set_contains("s", key),
              "set recovery");
        check(recovered.get("n") == "10" && recovered.get("x") == "last" &&
                  recovered.get("y") == "second",
              "integer and MSET recovery");
        check(recovered.remaining_ttl("ttl", true) == 5000 && recovered.accounted_bytes() == total,
              "absolute TTL and recomputed ledger");
    }
    clock.wall += 5s;
    {
        Database recovered(clock);
        AofLog log(directory.options(), clock);
        log.recover(recovered);
        check(!recovered.exists("ttl") && !recovered.exists("gone"),
              "downtime expiry not resurrected");
        apply(recovered, log, [&] { (void)recovered.erase(key); });
        apply(recovered, log, [&] { (void)recovered.list_pop("l", true); });
        apply(recovered, log, [&] { (void)recovered.hash_delete("h", {key, ""}); });
        apply(recovered, log, [&] { (void)recovered.set_remove("s", {key, ""}); });
        apply(recovered, log, [&] { (void)recovered.set("l", "overwritten"); });
    }
    Database final(clock);
    AofLog log(directory.options(), clock);
    log.recover(final);
    check(!final.exists(key) && !final.exists("h") && !final.exists("s") &&
              final.get("l") == "overwritten" && !final.exists("ttl"),
          "deletion and type overwrite recovery");
}

void memory_and_rollback() {
    Directory directory;
    Clock clock;
    const storage::MemoryOptions memory{132, storage::EvictionPolicy::AllKeysLru};
    {
        Database db(clock, memory);
        AofLog log(directory.options(), clock);
        log.recover(db);
        apply(db, log, [&] { (void)db.set("a", "x"); });
        apply(db, log, [&] { (void)db.set("b", "y"); });
        (void)db.get("a");
        // This data commit captures the preceding read's recency.
        apply(db, log, [&] { (void)db.set("b", "z"); });
        const auto sequence = log.sequence();
        fails([&] { apply(db, log, [&] { db.set_many({{"a", "xx"}, {"b", "yy"}}); }); });
        fails([&] { apply(db, log, [&] { (void)db.list_push("a", {"bad"}, true); }); });
        check(log.sequence() == sequence && db.accounted_bytes() == 132,
              "rejections not journaled");
        {
            Database::Transaction trial(db);
            (void)db.set("c", "z");
            check(trial.effects().entries.size() == 2,
                  "eviction represented as tombstone plus image");
        }
        check(db.get("a") == "x" && db.get("b") == "z" && !db.exists("c"), "transaction rollback");
    }
    {
        Database db(clock, memory);
        AofLog log(directory.options(), clock);
        log.recover(db);
        // Last durable order is a,b. Replay must not choose fresh eviction victims.
        apply(db, log, [&] { (void)db.set("c", "q"); });
        check(!db.exists("a") && db.get("b") == "z" && db.get("c") == "q", "LRU restored");
    }
    {
        Database db(clock, memory);
        AofLog log(directory.options(), clock);
        log.recover(db);
        check(!db.exists("a") && db.accounted_bytes() == 132, "eviction stays deleted on restart");
    }
    Database small(clock, storage::MemoryOptions{1});
    AofLog log(directory.options(), clock);
    fails([&] { log.recover(small); });
    check(log.failed(), "smaller recovery budget fails closed");
}

void policies_and_failures() {
    Clock clock;
    for (auto policy : {SyncPolicy::Always, SyncPolicy::EverySecond, SyncPolicy::No}) {
        Directory directory;
        Database db(clock);
        unsigned syncs = 0;
        persistence::AofIo io;
        io.sync = [&](int fd) {
            ++syncs;
            return ::fsync(fd);
        };
        AofLog log(directory.options(policy), clock, io);
        log.recover(db);
        const auto baseline = syncs;
        apply(db, log, [&] { (void)db.set("a", "x"); });
        check((syncs > baseline) == (policy == SyncPolicy::Always),
              "per-write synchronization policy");
        clock.mono += 2s;
        log.tick();
        check((syncs > baseline) == (policy != SyncPolicy::No), "scheduled synchronization policy");
        log.finish();
        check(syncs > baseline, "clean shutdown synchronizes pending journal");
    }
    for (bool sync_failure : {false, true}) {
        Directory directory;
        Database db(clock);
        bool fail = false, wrote_partial = false;
        persistence::AofIo io;
        io.sync = [&](int fd) {
            if (fail && sync_failure) {
                errno = EIO;
                return -1;
            }
            return ::fsync(fd);
        };
        io.write = [&](int fd, const void* data, std::size_t count) -> ssize_t {
            if (fail && !sync_failure) {
                if (!wrote_partial) {
                    wrote_partial = true;
                    return ::write(fd, data, std::min(count, std::size_t{3}));
                }
                errno = ENOSPC;
                return -1;
            }
            return ::write(fd, data, count);
        };
        {
            AofLog log(directory.options(), clock, io);
            log.recover(db);
            apply(db, log, [&] { (void)db.set("a", "old"); });
            fail = true;
            fails([&] { apply(db, log, [&] { (void)db.set("a", "new"); }); });
            check(log.failed() && db.get("a") == "old", "failed persistence rolls memory back");
            fails([&] { apply(db, log, [&] { (void)db.set("b", "forbidden"); }); });
            check(!db.exists("b"), "poisoned journal rejects future commits");
        }
        const auto bytes = read(directory.options().file);
        Database recovered(clock);
        AofLog log(directory.options(), clock);
        if (sync_failure) {
            log.recover(recovered);
            check(recovered.get("a") == "new",
                  "complete unacknowledged record may recover after failed sync");
        } else {
            fails([&] { log.recover(recovered); });
            check(read(directory.options().file) == bytes, "partial write never auto-truncated");
        }
    }
    {
        Directory directory;
        Database db(clock);
        bool fail = false;
        persistence::AofIo io;
        io.sync = [&](int fd) {
            if (fail) {
                errno = EIO;
                return -1;
            }
            return ::fsync(fd);
        };
        AofLog log(directory.options(SyncPolicy::EverySecond), clock, io);
        log.recover(db);
        apply(db, log, [&] { (void)db.set("a", "x"); });
        fail = true;
        clock.mono += 2s;
        fails([&] { log.tick(); });
        check(log.failed(), "periodic sync failure latches");
        fails([&] { log.finish(); });
    }
    {
        Directory directory;
        Database db(clock);
        bool interrupt = false;
        persistence::AofIo io;
        io.write = [&](int fd, const void* data, std::size_t count) -> ssize_t {
            interrupt = !interrupt;
            if (interrupt) {
                errno = EINTR;
                return -1;
            }
            return ::write(fd, data, std::min(count, std::size_t{5}));
        };
        AofLog log(directory.options(), clock, io);
        log.recover(db);
        apply(db, log, [&] { (void)db.set("a", "short-write-safe"); });
        check(db.get("a") == "short-write-safe", "short writes and EINTR retried");
    }
}

void corruption_and_ownership() {
    Directory directory;
    Clock clock;
    std::size_t first_end;
    {
        Database db(clock);
        AofLog log(directory.options(), clock);
        log.recover(db);
        fails([&] { AofLog second(directory.options(), clock); });
        apply(db, log, [&] { (void)db.set("a", "first"); });
        first_end = read(directory.options().file).size();
        apply(db, log, [&] { (void)db.set("b", "second"); });
    }
    const auto original = read(directory.options().file);
    const auto watermark = read(directory.options().file.string() + ".commit");
    for (std::size_t length = 0; length < original.size(); ++length) {
        write(directory.options().file, std::string_view(original).substr(0, length));
        const auto before = read(directory.options().file);
        fails([&] {
            Database db(clock);
            AofLog log(directory.options(), clock);
            log.recover(db);
        });
        check(read(directory.options().file) == before, "truncation changes journal");
    }
    check(first_end < original.size(), "two distinct records");
    for (std::size_t i :
         {std::size_t{0}, std::size_t{11}, std::size_t{20}, first_end + 20, original.size() - 1}) {
        auto damaged = original;
        damaged[i] ^= 1;
        write(directory.options().file, damaged);
        fails([&] {
            Database db(clock);
            AofLog log(directory.options(), clock);
            log.recover(db);
        });
        check(read(directory.options().file) == damaged, "corruption changes journal");
    }
    write(directory.options().file, original);
    auto damaged_mark = watermark;
    damaged_mark[16] ^= 1;
    write(directory.options().file.string() + ".commit", damaged_mark);
    fails([&] { AofLog log(directory.options(), clock); });
    check(persistence::crc32c("123456789") == 0xe3069283U, "CRC32C known vector");
    fails([&] { (void)persistence::decode_effects(std::string(8, '\xff')); });
    fails([&] { (void)persistence::decode_effects("\0"); });
}

void publication_failures() {
    // Fail each synchronization boundary: journal, temporary watermark,
    // directory after atomic rename. All must withhold publication and latch.
    for (unsigned failure_at : {1U, 2U, 3U}) {
        Directory directory;
        Clock clock;
        Database db(clock);
        unsigned calls = 0;
        bool inject = false;
        persistence::AofIo io;
        io.sync = [&](int fd) {
            if (inject && ++calls == failure_at) {
                errno = EIO;
                return -1;
            }
            return ::fsync(fd);
        };
        {
            AofLog log(directory.options(), clock, io);
            log.recover(db);
            apply(db, log, [&] { (void)db.set("a", "old"); });
            inject = true;
            fails([&] { apply(db, log, [&] { (void)db.set("a", "new"); }); });
            check(log.failed() && db.get("a") == "old",
                  "publication sync failure escaped rollback");
        }
        Database recovered(clock);
        AofLog log(directory.options(), clock);
        log.recover(recovered);
        check(recovered.get("a") == "new", "uncertain complete commit must remain recoverable");
    }
}

void expired_lru_restart() {
    Directory directory;
    Clock clock;
    const storage::MemoryOptions memory{181, storage::EvictionPolicy::AllKeysLru};
    {
        Database db(clock, memory);
        AofLog log(directory.options(), clock);
        log.recover(db);
        apply(db, log, [&] { (void)db.set_with_expiration("a", "x", 1, true); });
        apply(db, log, [&] { (void)db.set("b", "y"); });
    }
    clock.wall += 1ms;
    for (int cycle = 0; cycle < 3; ++cycle) {
        Database db(clock, memory);
        AofLog log(directory.options(), clock);
        log.recover(db);
        check(!db.exists("a") && db.get("b") == "y", "expired LRU restart state");
        apply(db, log, [&] { (void)db.set("c", std::to_string(cycle)); });
        check(db.accounted_bytes() == 132, "startup expiry ledger");
    }
}
} // namespace

int main() {
    try {
        roundtrip_and_cycles();
        memory_and_rollback();
        policies_and_failures();
        corruption_and_ownership();
        publication_failures();
        expired_lru_restart();
        std::cout << "PASS AOF codec, replay, TTL/LRU/ledger, policies, failure rollback and "
                     "corruption\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
