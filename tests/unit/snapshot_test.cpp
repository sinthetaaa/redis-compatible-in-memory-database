#include "redisdb/persistence/Snapshot.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <poll.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace redisdb;
using namespace std::chrono_literals;
using persistence::AofLog;
using persistence::SnapshotBoundary;
using persistence::SnapshotStore;
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
    storage::WallTimePoint wall_now() const noexcept override { return wall; }
    storage::MonotonicTimePoint monotonic_now() const noexcept override { return {}; }
};
struct Directory {
    std::filesystem::path path;
    Directory() {
        std::string name =
            (std::filesystem::temp_directory_path() / "redisdb-snapshot-XXXXXX").string();
        check(::mkdtemp(name.data()) != nullptr, "mkdtemp");
        path = name;
        std::filesystem::create_directory(path / "snapshots");
    }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    persistence::AofOptions aof() const {
        return {true, path / "db.aof", persistence::SyncPolicy::No};
    }
    persistence::SnapshotOptions snapshots() const { return {path / "snapshots"}; }
    std::filesystem::path data(unsigned generation) const {
        return path / "snapshots" / ("generation-" + std::to_string(generation) + ".snap");
    }
};
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(bool(file), "write fixture");
}
void put(std::string& bytes, std::size_t at, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        bytes[at + i] = static_cast<char>((value >> (8 * (width - i - 1))) & 255U);
    }
}
void fix_header(std::string& bytes) {
    put(bytes, 76, persistence::crc32c(std::string_view(bytes).substr(0, 76)), 4);
    put(bytes, bytes.size() - 12,
        persistence::crc32c(std::string_view(bytes).substr(0, bytes.size() - 20)), 4);
    put(bytes, bytes.size() - 8,
        persistence::crc32c(std::string_view(bytes).substr(bytes.size() - 20, 12)), 4);
}
template <class Fn> void apply(Database& db, AofLog& log, Fn fn) {
    Database::Transaction transaction(db);
    fn();
    log.append(transaction.effects());
    transaction.commit();
}
void seed(Directory& dir, Clock& clock) {
    Database db(clock);
    SnapshotStore store(dir.snapshots());
    const auto checkpoint = store.restore(db);
    AofLog log(dir.aof(), clock);
    log.recover(db, checkpoint);
    apply(db, log, [&] { (void)db.set("n", "1"); });
    check(store.publish(db, log) == 1, "initial generation");
}
void recover_two(Directory& dir, Clock& clock) {
    Database db(clock);
    SnapshotStore store(dir.snapshots());
    auto checkpoint = store.restore(db);
    AofLog log(dir.aof(), clock, {}, true);
    log.recover(db, checkpoint);
    check(db.get("n") == "2", "previous generation plus synchronized suffix must recover");
    const auto sequence = log.sequence();
    check(sequence == 2, "snapshot must not duplicate mutations");
}

void roundtrip() {
    Directory dir;
    Clock clock;
    const std::string binary("\0\xff\r\n", 4);
    const storage::MemoryOptions memory{{}, storage::EvictionPolicy::AllKeysLru};
    std::size_t charge = 0;
    {
        Database db(clock, memory);
        SnapshotStore store(dir.snapshots());
        auto checkpoint = store.restore(db);
        AofLog log(dir.aof(), clock);
        log.recover(db, checkpoint);
        apply(db, log, [&] {
            (void)db.set(binary, binary);
            (void)db.list_push("l", {"", binary, "tail"}, false);
            (void)db.hash_set("h", {{binary, binary}, {"", ""}});
            (void)db.set_add("s", {binary, ""});
            (void)db.adjust_integer("n", 42, false);
            (void)db.set_with_expiration("ttl", "v", 10000, true);
            (void)db.set_with_expiration("expired", "v", 100, true);
        });
        charge = db.accounted_bytes();
        check(db.get(binary) == binary, "read-only LRU touch");
        check(store.publish(db, log) == 1, "publish");
        const auto point = log.checkpoint();
        check(point.sequence == 1 && point.offset == std::filesystem::file_size(dir.aof().file),
              "checkpoint coordinates");
        apply(db, log, [&] { (void)db.adjust_integer("n", 1, false); });
        log.finish();
    }
    clock.wall += 200ms;
    for (int cycle = 0; cycle < 3; ++cycle) {
        Database db(clock, memory);
        SnapshotStore store(dir.snapshots());
        auto checkpoint = store.restore(db);
        check(db.accounted_bytes() == charge, "snapshot restores pre-expiry ledger");
        AofLog log(dir.aof(), clock, {}, true);
        log.recover(db, checkpoint);
        check(db.get(binary) == binary && db.get("n") == "43", "binary/integer plus AOF suffix");
        check(!db.exists("expired") && db.remaining_ttl("ttl", true) > 0, "absolute TTL recovery");
        check(db.list_range("l", 0, -1, 4096) == std::vector<std::string>({"", binary, "tail"}),
              "list order");
        check(db.hash_get("h", binary) == binary, "hash binary");
        check(db.set_cardinality("s") == 2, "set restore");
        check(log.sequence() == 3, "startup expiry journaled once");
    }
    // Empty snapshots are legal and generation monotonically advances.
    {
        Database db(clock);
        SnapshotStore store(dir.snapshots());
        auto cp = store.restore(db);
        AofLog log(dir.aof(), clock);
        log.recover(db, cp);
        apply(db, log, [&] { db.clear(); });
        check(store.publish(db, log) == 2, "second generation");
        check(std::filesystem::exists(dir.data(1)), "previous generation retained");
    }
    {
        Database db(clock);
        SnapshotStore store(dir.snapshots());
        auto cp = store.restore(db);
        AofLog log(dir.aof(), clock);
        log.recover(db, cp);
        check(db.size() == 0, "empty snapshot");
    }
}

void corruption_and_association() {
    Directory dir;
    Clock clock;
    seed(dir, clock);
    const auto original = read(dir.data(1));
    auto failure = [&] {
        const auto aof = read(dir.aof().file), mark = read(dir.aof().file.string() + ".commit");
        fails([&] {
            Database db(clock);
            SnapshotStore store(dir.snapshots());
            auto cp = store.restore(db);
            AofLog log(dir.aof(), clock, {}, true);
            log.recover(db, cp);
        });
        check(read(dir.aof().file) == aof && read(dir.aof().file.string() + ".commit") == mark,
              "invalid recovery changed journal");
    };
    for (std::size_t offset :
         {std::size_t{0}, std::size_t{8}, std::size_t{80}, original.size() - 1}) {
        auto bytes = original;
        bytes[offset] ^= 1;
        write(dir.data(1), bytes);
        failure();
    }
    for (std::size_t offset : {std::size_t{8}, std::size_t{12}, std::size_t{16}, std::size_t{24},
                               std::size_t{32}, std::size_t{40}, std::size_t{44}, std::size_t{48},
                               std::size_t{56}, std::size_t{64}, std::size_t{72}}) {
        auto bytes = original;
        bytes[offset] ^= 1;
        fix_header(bytes);
        write(dir.data(1), bytes);
        failure();
    }
    for (std::size_t length : {std::size_t{0}, std::size_t{79}, original.size() - 1}) {
        write(dir.data(1), original.substr(0, length));
        failure();
    }
    std::filesystem::remove(dir.data(1));
    failure();
    std::filesystem::create_symlink(dir.aof().file, dir.data(1));
    failure();
    std::filesystem::remove(dir.data(1));
    check(::mkfifo(dir.data(1).c_str(), 0600) == 0, "mkfifo");
    failure();
    std::filesystem::remove(dir.data(1));
    write(dir.data(1), original);
    // A snapshot cannot hide damaged history, even before its checkpoint.
    const auto journal = read(dir.aof().file);
    auto bad = journal;
    bad[30] ^= 1;
    write(dir.aof().file, bad);
    failure();
    write(dir.aof().file, journal);
    // Different valid journal with the same seq/offset/last-record size is rejected by prefix CRC.
    Directory other;
    {
        Database db(clock);
        AofLog log(other.aof(), clock);
        log.recover(db);
        apply(db, log, [&] { (void)db.set("n", "9"); });
        log.finish();
    }
    write(dir.aof().file, read(other.aof().file));
    write(dir.aof().file.string() + ".commit", read(other.aof().file.string() + ".commit"));
    failure();
    write(dir.aof().file, journal);
    // Missing journal must not be recreated when a snapshot selects a checkpoint.
    std::filesystem::remove(dir.aof().file);
    fails([&] { AofLog log(dir.aof(), clock, {}, true); });
    check(!std::filesystem::exists(dir.aof().file), "missing required journal recreated");
    const auto selector = dir.path / "snapshots" / "CURRENT";
    const auto current = read(selector);
    write(selector, current.substr(0, 31));
    fails([&] { SnapshotStore store(dir.snapshots()); });
    write(selector, current);
    std::filesystem::remove(selector);
    fails([&] { SnapshotStore store(dir.snapshots()); });
}

void failures_and_crash_boundaries() {
    // Every publication syscall can fail without changing the previous generation.
    for (int kind = 0; kind < 3; ++kind) {
        const int count = kind == 0 ? 4 : kind == 1 ? 4 : 2;
        for (int at = 1; at <= count; ++at) {
            Directory dir;
            Clock clock;
            seed(dir, clock);
            const auto previous = read(dir.data(1));
            {
                bool armed = false;
                int calls = 0;
                persistence::SnapshotIo io;
                io.file.write = [&](int fd, const void* p, std::size_t n) -> ssize_t {
                    if (armed && kind == 0 && ++calls == at) {
                        errno = ENOSPC;
                        return -1;
                    }
                    return ::write(fd, p, n);
                };
                io.file.sync = [&](int fd) {
                    if (armed && kind == 1 && ++calls == at) {
                        errno = EIO;
                        return -1;
                    }
                    return ::fsync(fd);
                };
                io.rename = [&](const char* from, const char* to) {
                    if (armed && kind == 2 && ++calls == at) {
                        errno = EIO;
                        return -1;
                    }
                    return ::rename(from, to);
                };
                Database db(clock);
                SnapshotStore store(dir.snapshots(), io);
                auto cp = store.restore(db);
                AofLog log(dir.aof(), clock);
                log.recover(db, cp);
                apply(db, log, [&] { (void)db.adjust_integer("n", 1, false); });
                armed = true;
                fails([&] { (void)store.publish(db, log); });
                check(calls >= at, "failure seam not exercised");
                fails([&] { (void)store.publish(db, log); });
                check(read(dir.data(1)) == previous, "previous generation altered");
            }
            recover_two(dir, clock);
        }
    }
    // Deterministic SIGKILL at ALL eight real-store publication boundaries.
    for (auto point : {SnapshotBoundary::DataWritten, SnapshotBoundary::DataSynced,
                       SnapshotBoundary::DataRenamed, SnapshotBoundary::DataDirectorySynced,
                       SnapshotBoundary::ManifestWritten, SnapshotBoundary::ManifestSynced,
                       SnapshotBoundary::ManifestRenamed, SnapshotBoundary::Published}) {
        Directory dir;
        Clock clock;
        seed(dir, clock);
        int notification[2];
        check(::pipe(notification) == 0, "pipe");
        const auto child = ::fork();
        check(child >= 0, "fork");
        if (child == 0) {
            (void)::close(notification[0]);
            try {
                persistence::SnapshotIo io;
                io.boundary = [&](SnapshotBoundary boundary) {
                    if (boundary == point) {
                        const char ready = '!';
                        if (::write(notification[1], &ready, 1) != 1)
                            ::_exit(3);
                        for (;;)
                            ::pause();
                    }
                };
                Database db(clock);
                SnapshotStore store(dir.snapshots(), io);
                auto cp = store.restore(db);
                AofLog log(dir.aof(), clock);
                log.recover(db, cp);
                apply(db, log, [&] { (void)db.adjust_integer("n", 1, false); });
                (void)store.publish(db, log);
                ::_exit(4);
            } catch (...) {
                ::_exit(5);
            }
        }
        (void)::close(notification[1]);
        char ready = 0;
        pollfd notification_poll{notification[0], POLLIN, 0};
        const auto got =
            ::poll(&notification_poll, 1, 10000) > 0 ? ::read(notification[0], &ready, 1) : -1;
        (void)::close(notification[0]);
        (void)::kill(child, SIGKILL);
        int status = 0;
        check(::waitpid(child, &status, 0) == child, "wait child");
        check(got == 1 && ready == '!' && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
              "crash seam");
        recover_two(dir, clock);
        // Retry after orphan candidate publication must allocate a fresh generation.
        Database db(clock);
        SnapshotStore store(dir.snapshots());
        auto cp = store.restore(db);
        AofLog log(dir.aof(), clock);
        log.recover(db, cp);
        check(store.publish(db, log) >= 2, "retry generation");
        check(std::filesystem::exists(dir.data(1)), "old generation lost after retry");
    }
}

void ownership_empty_and_policy_changes() {
    Directory dir;
    Clock clock;
    {
        Database db(clock);
        SnapshotStore store(dir.snapshots());
        fails([&] { SnapshotStore second(dir.snapshots()); });
        auto cp = store.restore(db);
        AofLog log(dir.aof(), clock);
        log.recover(db, cp);
        check(store.publish(db, log) == 1, "empty sequence-zero checkpoint");
    }
    {
        Database db(clock);
        SnapshotStore store(dir.snapshots());
        auto cp = store.restore(db);
        check(cp && cp->sequence == 0 && cp->offset == 20, "zero checkpoint restoration");
        AofLog log(dir.aof(), clock);
        log.recover(db, cp);
        apply(db, log, [&] { db.set_many({{"b", "y"}, {"a", "x"}}); });
        (void)store.publish(db, log);
    }
    // noeviction -> LRU uses sorted-key cold order, then persists read touches.
    const storage::MemoryOptions lru{132, storage::EvictionPolicy::AllKeysLru};
    {
        Database db(clock, lru);
        SnapshotStore store(dir.snapshots());
        auto cp = store.restore(db);
        AofLog log(dir.aof(), clock);
        log.recover(db, cp);
        check(db.snapshot_order().lru_order == std::vector<std::string>({"a", "b"}),
              "policy cold order");
        (void)db.get("a");
        (void)store.publish(db, log);
    }
    {
        Database db(clock, lru);
        SnapshotStore store(dir.snapshots());
        auto cp = store.restore(db);
        AofLog log(dir.aof(), clock);
        log.recover(db, cp);
        check(db.snapshot_order().lru_order == std::vector<std::string>({"b", "a"}),
              "snapshot read touches");
        apply(db, log, [&] { (void)db.set("c", "z"); });
        check(!db.exists("b") && db.accounted_bytes() == 132, "snapshot eviction/ledger");
        (void)store.publish(db, log);
    }
    {
        Database db(clock); // restore LRU-produced snapshot with noeviction
        SnapshotStore store(dir.snapshots());
        auto cp = store.restore(db);
        AofLog log(dir.aof(), clock);
        log.recover(db, cp);
        check(db.size() == 2 && db.get("a") == "x" && db.get("c") == "z", "policy switch recovery");
    }
    // Validate LRU key sets regardless of the current eviction policy.
    auto data = read(dir.data(4));
    const auto tag = data.find("LRU1");
    check(tag != std::string::npos, "LRU fixture");
    data[tag + 8] = '?';
    put(data, tag + 9, persistence::crc32c(std::string_view(data).substr(tag, 9)), 4);
    fix_header(data);
    write(dir.data(4), data);
    fails([&] {
        Database db(clock);
        SnapshotStore store(dir.snapshots());
        (void)store.restore(db);
    });

    Directory valid;
    for (const auto& path : {valid.snapshots().directory / "", valid.snapshots().directory / "."}) {
        SnapshotStore store({path});
        Database db(clock);
        check(!store.restore(db), "real directory with trailing components must remain valid");
    }
    Directory other;
    const auto alias = other.path / "alias";
    std::filesystem::create_directory_symlink(other.snapshots().directory, alias);
    fails([&] { SnapshotStore store({alias}); });
    // Trailing separators/dot components must not bypass final symlink rejection.
    fails([&] { SnapshotStore store({alias / ""}); });
    fails([&] { SnapshotStore store({alias / "."}); });
    fails([&] { SnapshotStore store({alias / "." / ""}); });
    const auto lock = other.snapshots().directory / "LOCK";
    check(::mkfifo(lock.c_str(), 0600) == 0, "FIFO lock fixture");
    fails([&] { SnapshotStore store(other.snapshots()); });
    std::filesystem::remove(lock);
    std::filesystem::create_symlink(other.aof().file, lock);
    fails([&] { SnapshotStore store(other.snapshots()); });
}

void checkpoint_failure_and_prefix_binding() {
    Directory dir;
    Clock clock;
    seed(dir, clock);
    const auto current = read(dir.snapshots().directory / "CURRENT");
    {
        bool armed = false;
        persistence::AofIo io;
        io.sync = [&](int fd) {
            if (armed) {
                errno = EIO;
                return -1;
            }
            return ::fsync(fd);
        };
        Database db(clock);
        SnapshotStore store(dir.snapshots());
        auto cp = store.restore(db);
        AofLog log(dir.aof(), clock, io);
        log.recover(db, cp);
        apply(db, log, [&] { (void)db.adjust_integer("n", 1, false); });
        armed = true;
        fails([&] { (void)store.publish(db, log); });
        check(log.failed(), "checkpoint failure must poison journal");
        check(read(dir.snapshots().directory / "CURRENT") == current,
              "failed AOF checkpoint selected snapshot");
        check(!std::filesystem::exists(dir.data(2)), "failed AOF checkpoint constructed snapshot");
    }
    recover_two(dir, clock);
    // Two valid histories have identical final record/coordinates but distinct prefixes.
    Directory first, second;
    for (auto* fixture : {&first, &second}) {
        Database db(clock);
        SnapshotStore store(fixture->snapshots());
        auto cp = store.restore(db);
        AofLog log(fixture->aof(), clock);
        log.recover(db, cp);
        apply(db, log, [&] { (void)db.set("x", fixture == &first ? "a" : "b"); });
        apply(db, log, [&] { (void)db.set("n", "1"); });
        (void)store.publish(db, log);
    }
    const auto a = read(first.data(1)), b = read(second.data(1));
    check(a.substr(24, 20) == b.substr(24, 20) && a.substr(44, 4) != b.substr(44, 4),
          "prefix-binding fixture");
    write(first.aof().file, read(second.aof().file));
    write(first.aof().file.string() + ".commit", read(second.aof().file.string() + ".commit"));
    bool rejected = false;
    try {
        Database db(clock);
        SnapshotStore store(first.snapshots());
        auto cp = store.restore(db);
        AofLog log(first.aof(), clock);
        log.recover(db, cp);
    } catch (const persistence::PersistenceError& error) {
        rejected =
            std::string_view(error.what()).find("checkpoint disagrees") != std::string_view::npos;
    }
    check(rejected, "full prefix association was not checked");
}

void short_writes_and_large_snapshot() {
    Directory dir;
    Clock clock;
    unsigned writes = 0, syncs = 0;
    persistence::SnapshotIo io;
    io.file.write = [&](int fd, const void* data, std::size_t n) -> ssize_t {
        if (++writes % 7 == 1) {
            errno = EINTR;
            return -1;
        }
        return ::write(fd, data, std::min(n, std::size_t{32768}));
    };
    io.file.sync = [&](int fd) {
        if (++syncs % 3 == 1) {
            errno = EINTR;
            return -1;
        }
        return ::fsync(fd);
    };
    {
        Database db(clock);
        SnapshotStore store(dir.snapshots(), io);
        auto cp = store.restore(db);
        AofLog log(dir.aof(), clock);
        log.recover(db, cp);
        const std::string value(8 * 1024 * 1024, 'x');
        for (int i = 0; i < 9; ++i) {
            apply(db, log, [&] { (void)db.set(std::to_string(i), value); });
        }
        check(store.publish(db, log) == 1, "large snapshot publication");
        check(std::filesystem::file_size(dir.data(1)) > persistence::MaxAofRecordBytes,
              "snapshot incorrectly capped at AOF transaction limit");
    }
    Database db(clock);
    SnapshotStore store(dir.snapshots());
    auto cp = store.restore(db);
    AofLog log(dir.aof(), clock);
    log.recover(db, cp);
    check(db.size() == 9 && db.get("8")->size() == 8 * 1024 * 1024, "large snapshot recovery");
}
} // namespace
int main() {
    try {
        roundtrip();
        corruption_and_association();
        failures_and_crash_boundaries();
        ownership_empty_and_policy_changes();
        checkpoint_failure_and_prefix_binding();
        short_writes_and_large_snapshot();
        std::cout
            << "PASS snapshots: types/TTL/ledger, association/corruption, 10 syscall failures, "
               "8 SIGKILL boundaries, EINTR/short writes, >64MiB snapshot\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
