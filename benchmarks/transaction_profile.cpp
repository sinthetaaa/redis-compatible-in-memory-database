// Small phase/allocation profiler for the real Database/AOF path, not a throughput test.
#include "redisdb/persistence/AofLog.hpp"
#include "redisdb/storage/Database.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

namespace {
bool counting = false;
std::size_t allocations = 0, allocated_bytes = 0;
using Timer = std::chrono::steady_clock;
struct Phase {
    double ns = 0;
    std::size_t calls = 0, bytes = 0;
};
template <class F> void measure(Phase& phase, F fn) {
    allocations = allocated_bytes = 0;
    const auto start = Timer::now();
    counting = true;
    fn();
    counting = false;
    phase.ns += std::chrono::duration<double, std::nano>(Timer::now() - start).count();
    phase.calls += allocations;
    phase.bytes += allocated_bytes;
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "redisdb-profile-XXXXXX").string();
        if (!::mkdtemp(pattern.data()))
            throw std::runtime_error("mkdtemp");
        path = pattern;
    }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
class ProfileClock final : public redisdb::storage::Clock {
  public:
    redisdb::storage::SystemClock real;
    redisdb::storage::WallTimePoint wall = real.wall_now();
    redisdb::storage::WallTimePoint wall_now() const noexcept override { return wall; }
    redisdb::storage::MonotonicTimePoint monotonic_now() const noexcept override {
        return real.monotonic_now();
    }
};
} // namespace
void* operator new(std::size_t size) {
    if (void* p = std::malloc(size ? size : 1)) {
        if (counting) {
            ++allocations;
            allocated_bytes += size;
        }
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

int main(int argc, char** argv) {
    using namespace redisdb;
    try {
        if (argc != 6)
            throw std::runtime_error("usage: profiler KEYS ITERATIONS no|always|everysec "
                                     "noeviction|lru set|write80|expire");
        const auto keys = std::stoul(argv[1]), iterations = std::stoul(argv[2]);
        if (!keys || keys > 10000 || !iterations || iterations > 10000)
            throw std::runtime_error("bounded keys/iterations required");
        const std::string policy = argv[3], eviction = argv[4], workload = argv[5];
        if (policy != "no" && policy != "always" && policy != "everysec")
            throw std::runtime_error("policy");
        if (eviction != "noeviction" && eviction != "lru")
            throw std::runtime_error("eviction");
        if (workload != "set" && workload != "write80" && workload != "expire")
            throw std::runtime_error("workload");
        if (workload == "expire" && iterations > keys)
            throw std::runtime_error("expiry iterations must not exceed key count");
        ProfileClock clock;
        storage::MemoryOptions options;
        if (eviction == "lru")
            options.policy = storage::EvictionPolicy::AllKeysLru;
        storage::Database db(clock, options);
        Directory directory;
        double write_ns = 0, sync_ns = 0;
        std::size_t writes = 0, syncs = 0;
        persistence::AofIo io;
        io.write = [&](int fd, const void* data, std::size_t count) {
            const auto start = Timer::now();
            const auto result = ::write(fd, data, count);
            write_ns += std::chrono::duration<double, std::nano>(Timer::now() - start).count();
            ++writes;
            return result;
        };
        io.sync = [&](int fd) {
            const auto start = Timer::now();
            const auto result = ::fsync(fd);
            sync_ns += std::chrono::duration<double, std::nano>(Timer::now() - start).count();
            ++syncs;
            return result;
        };
        const auto sync_policy = policy == "always" ? persistence::SyncPolicy::Always
                                 : policy == "no"   ? persistence::SyncPolicy::No
                                                    : persistence::SyncPolicy::EverySecond;
        persistence::AofLog log({true, directory.path / "db.aof", sync_policy}, clock, io);
        log.recover(db);
        std::vector<std::pair<std::string, std::string>> seed;
        for (std::size_t i = 0; i < keys; ++i)
            seed.emplace_back("k" + std::to_string(i), std::string(64, 'z'));
        {
            storage::Database::Transaction tx(db);
            db.set_many(seed);
            if (workload == "expire") {
                for (const auto& [key, value] : seed) {
                    (void)value;
                    (void)db.set_expiration(key, clock.wall + std::chrono::milliseconds{1});
                }
            }
            log.append(tx.effects());
            tx.commit();
        }
        log.finish();
        if (workload == "expire")
            clock.wall += std::chrono::milliseconds{2};
        writes = syncs = 0;
        write_ns = sync_ns = 0;
        std::array<Phase, 6> phases{};
        for (std::size_t i = 0; i < iterations; ++i) {
            std::unique_ptr<storage::Database::Transaction> tx;
            storage::CommittedEffects effects;
            const auto key = "k" + std::to_string(i % keys);
            auto value = std::to_string(i);
            value.resize(64, 'x');
            if (workload == "write80" && i % 5 == 0) {
                measure(phases[1], [&] {
                    if (!db.get(key))
                        std::abort();
                });
                continue;
            }
            measure(phases[0], [&] { tx = std::make_unique<storage::Database::Transaction>(db); });
            measure(phases[1], [&] {
                if (workload == "expire") {
                    if (db.sweep_expired(1).deleted != 1)
                        std::abort();
                } else {
                    (void)db.set(key, value);
                }
            });
            measure(phases[2], [&] { effects = tx->effects(); });
            measure(phases[3], [&] {
                const auto encoded = persistence::encode_effects(effects);
                if (encoded.empty())
                    std::abort();
            });
            measure(phases[4], [&] {
                log.append(effects);
                log.tick();
            });
            measure(phases[5], [&] {
                tx->commit();
                tx.reset();
            });
        }
        log.finish();
        rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) != 0)
            throw std::runtime_error("getrusage");
        std::cout << "{\"keys\":" << keys << ",\"iterations\":" << iterations << ",\"policy\":\""
                  << policy << "\",\"eviction\":\"" << eviction << "\",\"workload\":\"" << workload
                  << "\",\"logical_bytes\":" << db.accounted_bytes() << ",\"phases\":{";
        const std::array<const char*, 6> names = {
            "begin",         "mutation", "effects", "encode_diagnostic", "append_including_encode",
            "commit_destroy"};
        for (std::size_t i = 0; i < phases.size(); ++i) {
            if (i)
                std::cout << ',';
            std::cout << '"' << names[i]
                      << "\":{\"ns_per_op\":" << phases[i].ns / static_cast<double>(iterations)
                      << ",\"allocations_per_op\":"
                      << static_cast<double>(phases[i].calls) / static_cast<double>(iterations)
                      << ",\"allocated_bytes_per_op\":"
                      << static_cast<double>(phases[i].bytes) / static_cast<double>(iterations)
                      << '}';
        }
        std::cout << "},\"write_calls\":" << writes << ",\"sync_calls\":" << syncs
                  << ",\"write_ns\":" << write_ns << ",\"sync_ns\":" << sync_ns
                  << ",\"maxrss_platform_units\":" << usage.ru_maxrss << "}\n";
    } catch (const std::exception& error) {
        counting = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
