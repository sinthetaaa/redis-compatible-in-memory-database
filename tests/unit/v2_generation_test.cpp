#include "redisdb/persistence/V2Generation.hpp"
#include "redisdb/persistence/V2Staging.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <poll.h>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
long allocation_budget = -1;
std::size_t allocation_rejections = 0, guarded_allocations = 0;
} // namespace
void* operator new(std::size_t size) {
    if (allocation_budget >= 0)
        ++guarded_allocations;
    if (allocation_budget == 0) {
        ++allocation_rejections;
        throw std::bad_alloc{};
    }
    if (allocation_budget > 0)
        --allocation_budget;
    if (auto* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc{};
}
[[gnu::noinline]] void operator delete(void* p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace redisdb::persistence::v2;
static_assert(!std::is_copy_constructible_v<RootLease>);
static_assert(!std::is_copy_constructible_v<PinnedFile>);
static_assert(std::is_nothrow_move_constructible_v<RootLease>);
static_assert(std::is_nothrow_move_assignable_v<RootLease>);
static_assert(std::is_nothrow_move_constructible_v<PinnedFile>);
static_assert(std::is_nothrow_move_assignable_v<PinnedFile>);
static_assert(std::is_nothrow_destructible_v<PinnedFile>);
static_assert(!std::is_constructible_v<GenerationId, int>);
template <class T>
concept PublicRawOpen = requires(T& io) { io.open("", 0); };
static_assert(!PublicRawOpen<RootIo>);
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
GenerationId gen(std::uint64_t n = 1) { return GenerationId{n}; }
static_assert(!std::is_copy_constructible_v<StagedFile>);
static_assert(std::is_nothrow_move_constructible_v<StagedFile>);
static_assert(std::is_nothrow_move_assignable_v<StagedFile>);
static_assert(std::is_nothrow_destructible_v<StagedFile>);
template <class T>
concept PublicCreate = requires(T& io) { io.create_at(0, "", 0, 0600); };
static_assert(!PublicCreate<RootIo>);
static_assert(static_cast<int>(RootOperation::Close) == 7);
static_assert(static_cast<int>(RootRole::Watermark) == 5);
static_assert(static_cast<int>(RootErrorCode::DependentsAlive) == 13);
// This harness alone creates/modifies/deletes fixtures. Never accepts a user path.
// Cleanup verifies the original directory inode, enumerates only this flat root,
// never follows links, and refuses unexpected nonempty subdirectories.
struct Fixture {
    std::filesystem::path path;
    dev_t device{};
    ino_t inode{};
    Fixture() {
        std::array<char, 64> name{};
        std::strcpy(name.data(), "/tmp/redisdb-v2-root-XXXXXX");
        check(::mkdtemp(name.data()) != nullptr, "fixture mkdtemp");
        path = name.data();
        struct stat st{};
        check(::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode), "fixture identity");
        device = st.st_dev;
        inode = st.st_ino;
        put("LOCK", "");
    }
    ~Fixture() noexcept {
        struct stat st{};
        if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_dev != device ||
            st.st_ino != inode)
            std::abort();
        const auto fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            std::abort();
        auto* dir = ::fdopendir(fd);
        if (!dir)
            std::abort();
        while (auto* entry = ::readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
                continue;
            struct stat info{};
            if (::fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0 ||
                ::unlinkat(fd, entry->d_name, S_ISDIR(info.st_mode) ? AT_REMOVEDIR : 0) != 0)
                std::abort();
        }
        (void)::closedir(dir);
        if (::rmdir(path.c_str()) != 0)
            std::abort();
    }
    void put(const char* name, std::string_view bytes) const {
        const auto p = path / name;
        const auto fd =
            ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        check(fd >= 0, "fixture exclusive create");
        while (!bytes.empty()) {
            const auto n = ::write(fd, bytes.data(), bytes.size());
            check(n > 0, "fixture write");
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
        check(::close(fd) == 0, "fixture close");
    }
    void remove(const char* name) const {
        check(::unlink((path / name).c_str()) == 0, "fixture unlink");
    }
    void files() const {
        put("CURRENT", std::string(112, '?')); // deliberately NOT a decoded V2 selector
        put("base-1.snap", std::string(204, 'b'));
        put("tail-1.aof", std::string(96, 't'));
        put("tail-1.commit", std::string(112, 'm'));
    }
    using Inventory = std::map<std::string, std::pair<std::uint64_t, std::string>>;
    Inventory inventory() const {
        Inventory result;
        auto* dir = ::opendir(path.c_str());
        check(dir != nullptr, "fixture inventory");
        while (auto* entry = ::readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
                continue;
            const auto p = path / entry->d_name;
            struct stat st{};
            check(::lstat(p.c_str(), &st) == 0, "inventory stat");
            std::string bytes;
            if (S_ISREG(st.st_mode)) {
                const auto fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                check(fd >= 0, "inventory open");
                std::array<char, 4096> buffer{};
                for (;;) {
                    const auto n = ::read(fd, buffer.data(), buffer.size());
                    check(n >= 0, "inventory read");
                    if (!n)
                        break;
                    bytes.append(buffer.data(), static_cast<std::size_t>(n));
                }
                check(::close(fd) == 0, "inventory close");
            } else if (S_ISLNK(st.st_mode)) {
                std::array<char, 1024> buffer{};
                const auto n = ::readlink(p.c_str(), buffer.data(), buffer.size());
                check(n >= 0, "inventory symlink");
                bytes.assign(buffer.data(), static_cast<std::size_t>(n));
            }
            // Independent FNV-1a fingerprint PLUS exact bytes, not a durability
            // or authentication claim. Include type, identity, size and links.
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto c : bytes)
                hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
            const auto metadata = std::to_string(st.st_dev) + "/" + std::to_string(st.st_ino) +
                                  "/" + std::to_string(st.st_mode) + "/" +
                                  std::to_string(st.st_nlink) + "/" + std::to_string(st.st_size);
            result.emplace(entry->d_name, std::make_pair(hash, metadata + ":" + bytes));
        }
        (void)::closedir(dir);
        return result;
    }
};

static_assert(!std::is_copy_constructible_v<GenerationCandidate>);
static_assert(!std::is_copy_assignable_v<GenerationCandidate>);
static_assert(std::is_nothrow_move_constructible_v<GenerationCandidate>);
static_assert(std::is_nothrow_move_assignable_v<GenerationCandidate>);
static_assert(std::is_nothrow_destructible_v<GenerationCandidate>);
static_assert(static_cast<int>(RootErrorCode::GenerationExhausted) == 16);
static_assert(static_cast<int>(RootErrorCode::SequenceExhausted) == 17);
static_assert(static_cast<int>(RootErrorCode::CandidateOccupied) == 18);
static_assert(static_cast<int>(RootOperation::UnlinkAt) == 12);
static_assert(static_cast<int>(RootRole::StagedWatermark) == 8);
template <class T>
concept RawDescriptor = requires(T& owner) { owner.fd(); };
template <class T>
concept Initialization = requires(RootLease& root) { T::first_for_explicit_initialization(root); };
static_assert(!RawDescriptor<GenerationCandidate> && !Initialization<GenerationCandidate>);

// Bounded adapter storage; no allocation/throw inside noexcept production hooks.
// Call boundaries are recorded independently of the Before/After observers.
struct Adapter final : RootIo {
    struct Descriptor {
        int fd = -1;
        RootRole role = RootRole::Root;
    };
    struct Trace {
        RootIoEvent event{};
        std::array<char, 70> name{};
        int fd = -1;
    };
    std::array<Descriptor, 16> descriptors{};
    std::array<Trace, 512> trace{};
    std::size_t used = 0, target = 0, reached = 0, named_calls = 0, artifact_probes = 0;
    std::size_t opens = 0, closes = 0, creates = 0, writes = 0, reads = 0, syncs = 0, unlinks = 0;
    RootRole active = RootRole::Root;
    bool candidate_only = false, flags_ok = true, tracking_ok = true;
    int fault_errno = EIO, clobber_errno = 0;
    // One scripted actual candidate lookup return at the chosen probe ordinal.
    std::size_t answer_at = 0, interrupts_remaining = 0;
    int following_errno = 0;
    bool fail_create = false;
    int answer_result = -1, answer_errno = EIO;
    // Real harness mutation at a selected candidate probe, before the syscall.
    const Fixture* fixture = nullptr;
    std::size_t create_at_probe = 0;
    const char* create_name = "base-2.snap";
    // Simulated occupied triples avoid thousands of fixture entries.
    std::uint64_t occupied_through = 0;
    bool all_occupied = false;
    RootRole edit_role = static_cast<RootRole>(99);
    enum class Edit { Inode, Device, Type, Links, Size } edit = Edit::Inode;
    bool edit_named_lock = false;
    std::size_t edit_root_after_probe = 0;
    bool close_fault = false, reuse_close = false;
    RootRole close_role = RootRole::LockFile;
    RootStage close_stage = RootStage::Call;
    int close_errno = EIO, replacement = -1;
    std::size_t close_hits = 0;
    RootRole role_of(int fd) const noexcept {
        for (const auto& d : descriptors)
            if (d.fd == fd)
                return d.role;
        return active;
    }
    std::size_t live() const noexcept {
        std::size_t n = 0;
        for (const auto& d : descriptors)
            if (d.fd >= 0)
                ++n;
        return n;
    }
    void adopt(int fd) noexcept {
        if (fd < 0)
            return;
        ++opens;
        for (auto& d : descriptors)
            if (d.fd < 0) {
                d = {fd, active};
                return;
            }
        std::abort();
    }
    bool record(RootIoEvent e, int fd = -1, const char* name = nullptr) noexcept {
        if (used < trace.size()) {
            trace[used].event = e;
            trace[used].fd = fd;
            if (name) {
                const auto n = std::strlen(name);
                if (n >= trace[used].name.size())
                    std::abort();
                std::memcpy(trace[used].name.data(), name, n + 1);
            }
        }
        ++used;
        if (target && used == target) {
            ++reached;
            return true;
        }
        return false;
    }
    bool call(RootOperation op, RootRole role, int fd = -1, const char* name = nullptr) noexcept {
        if (record({op, role, RootStage::Call, 0, 0}, fd, name)) {
            errno = fault_errno;
            return false;
        }
        return true;
    }
    void reset() noexcept {
        used = target = reached = artifact_probes = 0;
        trace.fill({});
    }
    void dump(const char* label) const {
        std::cout << "TRACE " << label << " boundaries=" << used << " reached=" << reached
                  << " live=" << live() << " probes=" << artifact_probes << '\n';
        for (std::size_t i = 0; i < std::min(used, trace.size()); ++i) {
            const auto& t = trace[i];
            std::cout << i + 1 << ':' << static_cast<int>(t.event.operation) << '/'
                      << static_cast<int>(t.event.role) << '/' << static_cast<int>(t.event.stage)
                      << " result=" << t.event.result << " errno=" << t.event.system_errno
                      << " fd=" << t.fd << " name=" << t.name.data() << '\n';
        }
    }
    int observe(const RootIoEvent& e) noexcept override {
        active = e.role;
        const bool trip = record(e);
        if (close_fault && e.operation == RootOperation::Close && e.role == close_role &&
            e.stage == close_stage) {
            ++close_hits;
            return close_errno;
        }
        if (e.stage == RootStage::After && clobber_errno)
            errno = clobber_errno;
        return trip ? fault_errno : 0;
    }
    int open(const char* path, int flags) noexcept override {
        if (candidate_only)
            std::abort();
        if (!call(RootOperation::Open, active))
            return -1;
        const auto fd = RootIo::open(path, flags);
        adopt(fd);
        return fd;
    }
    int open_at(int dir, const char* name, int flags) noexcept override {
        if (candidate_only)
            std::abort();
        if (!call(RootOperation::OpenAt, active, dir, name))
            return -1;
        const auto fd = RootIo::open_at(dir, name, flags);
        adopt(fd);
        return fd;
    }
    int stat(int fd, struct stat* info) noexcept override {
        const auto role = role_of(fd);
        if (!call(RootOperation::Stat, role, fd))
            return -1;
        const auto result = RootIo::stat(fd, info);
        if (result == 0 && (role == edit_role || (role == RootRole::Root && edit_root_after_probe &&
                                                  artifact_probes >= edit_root_after_probe))) {
            switch (edit) {
            case Edit::Inode:
                ++info->st_ino;
                break;
            case Edit::Device:
                ++info->st_dev;
                break;
            case Edit::Type:
                info->st_mode = role == RootRole::Root ? S_IFREG : S_IFDIR;
                break;
            case Edit::Links:
                info->st_nlink = 2;
                break;
            case Edit::Size:
                ++info->st_size;
                break;
            }
        }
        return result;
    }
    int named_stat(int dir, const char* name, struct stat* info, int flags) noexcept override {
        ++named_calls;
        flags_ok = flags_ok && flags == AT_SYMLINK_NOFOLLOW && std::strchr(name, '/') == nullptr;
        const bool artifact =
            active == RootRole::Base || active == RootRole::Tail || active == RootRole::Watermark;
        if (artifact) {
            if (std::strlen(name) > 32)
                std::abort();
            ++artifact_probes;
            if (fixture && create_at_probe == artifact_probes) {
                // Test-only action on a harness-owned flat root, never a production hook.
                const int fd = ::openat(dir, create_name,
                                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
                if (fd < 0 || ::close(fd) != 0)
                    std::abort();
            }
        }
        if (!call(RootOperation::NamedStat, active, dir, name))
            return -1;
        if (artifact && interrupts_remaining) {
            --interrupts_remaining;
            errno = EINTR;
            return -1;
        }
        if (artifact && following_errno) {
            errno = std::exchange(following_errno, 0);
            return -1;
        }
        if (artifact && answer_at == artifact_probes) {
            errno = answer_errno;
            return answer_result;
        }
        if (artifact && (all_occupied || occupied_through)) {
            const char* start = name + 5;
            std::uint64_t n = 0;
            while (*start >= '0' && *start <= '9')
                n = n * 10 + static_cast<unsigned>(*start++ - '0');
            if (all_occupied || n <= occupied_through) {
                errno = ENOSPC;
                return 0;
            }
        }
        const auto result = RootIo::named_stat(dir, name, info, flags);
        if (result == 0 && edit_named_lock && std::strcmp(name, "LOCK") == 0)
            ++info->st_ino;
        return result;
    }
    int lock(int fd, int flags) noexcept override {
        if (candidate_only)
            std::abort();
        if (!call(RootOperation::Lock, active, fd))
            return -1;
        return RootIo::lock(fd, flags);
    }
    int create_at(int dir, const char* name, int flags, mode_t mode) noexcept override {
        if (candidate_only)
            std::abort();
        ++creates;
        if (!call(RootOperation::CreateAt, active, dir))
            return -1;
        if (fail_create) {
            ++reached;
            errno = ENOSPC;
            return -1;
        }
        const auto fd = RootIo::create_at(dir, name, flags, mode);
        adopt(fd);
        return fd;
    }
    int random_128(std::array<std::byte, 16>& token) noexcept override {
        if (candidate_only)
            std::abort();
        if (!call(RootOperation::Random128, active))
            return -1;
        return RootIo::random_128(token);
    }
    ssize_t write_at(int fd, const void* data, std::size_t n, off_t offset) noexcept override {
        if (candidate_only)
            std::abort();
        ++writes;
        if (!call(RootOperation::WriteAt, active, fd))
            return -1;
        return RootIo::write_at(fd, data, n, offset);
    }
    ssize_t read(int fd, void* data, std::size_t n, off_t offset) noexcept override {
        if (candidate_only)
            std::abort();
        ++reads;
        if (!call(RootOperation::Read, active, fd))
            return -1;
        return RootIo::read(fd, data, n, offset);
    }
    int sync_file(int fd) noexcept override {
        if (candidate_only)
            std::abort();
        ++syncs;
        if (!call(RootOperation::SyncFile, active, fd))
            return -1;
        return RootIo::sync_file(fd);
    }
    int unlink_at(int dir, const char* name) noexcept override {
        if (candidate_only)
            std::abort();
        ++unlinks;
        if (!call(RootOperation::UnlinkAt, active, dir))
            return -1;
        return RootIo::unlink_at(dir, name);
    }
    int close(int fd) noexcept override {
        const auto role = role_of(fd);
        ++closes;
        bool owned = false;
        for (auto& d : descriptors)
            if (d.fd == fd) {
                d.fd = -1;
                owned = true;
                break;
            }
        tracking_ok = tracking_ok && owned;
        const bool allowed = call(RootOperation::Close, role, fd);
        const auto result = RootIo::close(fd); // ALWAYS consume once even Before/Call rejection.
        if (close_fault && role == close_role && close_stage == RootStage::Call) {
            ++close_hits;
            if (reuse_close) {
                replacement = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                if (replacement != fd)
                    std::abort();
            }
            errno = close_errno;
            return -1;
        }
        if (!allowed) {
            errno = fault_errno;
            return -1;
        }
        return result;
    }
};

std::size_t scenarios = 0, injections = 0;
LogicalSequence seq(std::uint64_t n = 0) { return LogicalSequence{n}; }
GenerationCandidate find(RootLease& root, std::uint64_t selected = 1, std::uint32_t budget = 8,
                         std::uint64_t sequence = 0) {
    return GenerationCandidate::find_after(root, gen(selected), seq(sequence), budget);
}
void expected_error(RootError e, RootErrorCode code, RootOperation op = RootOperation::None,
                    RootRole role = RootRole::Root, RootStage stage = RootStage::Call,
                    int err = 0) {
    check(e == RootError{code, op, role, stage, err}, "exact error context");
}
void terminal(GenerationCandidate& candidate, RootLease& root, Adapter& io) {
    const auto first = candidate.error();
    check(first.code != RootErrorCode::None && !candidate.valid() && !candidate.generation() &&
              root.error() == first && !root.valid(),
          "shared terminal error");
    const auto used = io.used;
    check(!candidate.recheck_unused() && !find(root).valid() && io.used == used &&
              candidate.error() == first && root.error() == first,
          "terminal probes or lost first error");
    auto invalid = find(root, 1, 0);
    check(invalid.error() == first && io.used == used, "invalid budget replaced prior error");
    std::cout << "FIRST_ERROR " << static_cast<int>(first.code) << '/'
              << static_cast<int>(first.operation) << '/' << static_cast<int>(first.role) << '/'
              << static_cast<int>(first.stage) << '/' << first.system_errno << '\n';
}
template <class Fn> void scenario(const std::string& name, Fn fn) {
    Fixture f;
    f.files();
    f.put("legacy.aof", std::string("V1\0aof", 6));
    f.put("legacy.aof.commit", "V1 marker");
    f.put("generation-1.snap", "V1 snapshot");
    const auto before = f.inventory();
    auto io = std::make_shared<Adapter>();
    {
        auto root = RootLease::open_existing(f.path, io);
        check(root.valid() && io->live() == 2, "root fixture");
        fn(f, io, root);
    }
    check(io->live() == 0 && io->opens == io->closes && io->tracking_ok && io->flags_ok,
          "descriptor/flag accounting");
    const auto after = f.inventory();
    for (const auto& [path, bytes] : before) {
        const auto it = after.find(path);
        check(it != after.end() && it->second == bytes, "LOCK/CURRENT/selected/V1 preservation");
    }
    ++scenarios;
    std::cout << "SCENARIO " << scenarios << " PASS " << name
              << " preservation=LOCK,CURRENT,base,tail,watermark,V1 bytes+membership; live=0\n";
}
std::vector<std::string> probe_names(const Adapter& io) {
    std::vector<std::string> result;
    check(io.used <= io.trace.size(), "complete name trace required");
    for (std::size_t i = 0; i < io.used; ++i) {
        const auto& t = io.trace[i];
        if (t.event.operation == RootOperation::NamedStat && t.event.stage == RootStage::Call &&
            t.event.role >= RootRole::Base && t.event.role <= RootRole::Watermark)
            result.emplace_back(t.name.data());
    }
    return result;
}

void positives_and_names() {
    for (const auto selected :
         {std::uint64_t{1}, std::uint64_t{9}, std::uint64_t{99}, std::uint64_t{999},
          std::uint64_t{9999999999999999999ULL}, UINT64_MAX - 1}) {
        scenario("canonical successor " + std::to_string(selected), [=](auto& f, auto& io,
                                                                        auto& root) {
            const auto before = f.inventory();
            io->reset();
            io->candidate_only = true;
            auto c = find(root, selected, 1, 17);
            check(c.valid() && c.generation()->value() == selected + 1 && c.error() == RootError{},
                  "successor");
            const auto n = std::to_string(selected + 1);
            const std::vector<std::string> triple{"base-" + n + ".snap", "tail-" + n + ".aof",
                                                  "tail-" + n + ".commit"};
            auto expected = triple;
            expected.insert(expected.end(), triple.begin(), triple.end());
            check(probe_names(*io) == expected && io->live() == 2,
                  "exact all-role/confirmation names");
            for (std::size_t i = 0; i < triple.size(); ++i)
                check(triple[i].size() <= (i == 0 ? 30U : i == 1 ? 29U : 32U), "name bounds");
            if (selected == UINT64_MAX - 1)
                check(triple[0].size() == 30 && triple[1].size() == 29 && triple[2].size() == 32,
                      "maximum lengths");
            const auto used = io->used;
            check(c.recheck_unused() && io->used > used, "uncached recheck");
            auto duplicate = find(root, selected, 1);
            check(duplicate.valid() && *duplicate.generation() == *c.generation(),
                  "same-lease duplicate observations allowed");
            check(f.inventory() == before, "search/recheck modified membership");
            io->dump("canonical successor");
        });
    }
    for (const auto* name : {"base-2.snap", "tail-2.aof", "tail-2.commit"}) {
        scenario(std::string("single occupied role ") + name, [=](auto& f, auto& io, auto& root) {
            f.put(name, "");
            const auto before = f.inventory();
            io->reset();
            io->candidate_only = true;
            auto c = find(root);
            check(c.valid() && c.generation()->value() == 3 && io->artifact_probes == 9,
                  "skip occupied triple");
            auto names = probe_names(*io);
            check(names[0] == "base-2.snap" && names[1] == "tail-2.aof" &&
                      names[2] == "tail-2.commit",
                  "inspect all3 occupied triple");
            check(f.inventory() == before, "unowned occupied bytes/membership");
        });
    }
    scenario("multiple occupied generations and gaps", [](auto& f, auto& io, auto& root) {
        f.put("tail-2.commit", "partial");
        f.put("base-3.snap", "x");
        f.put("tail-4.aof", "bad");
        f.put("base-50.snap", "unrelated higher");
        const auto before = f.inventory();
        io->candidate_only = true;
        auto c = find(root, 1, 4);
        check(c.valid() && c.generation()->value() == 5 && io->artifact_probes == 15,
              "first eligible gap");
        check(f.inventory() == before, "occupied generations preserved");
    });
    for (int kind = 0; kind < 9; ++kind) {
        scenario("nonfollowing occupied entry kind " + std::to_string(kind),
                 [=](auto& f, auto& io, auto& root) {
                     int socket_fd = -1;
                     const auto p = f.path / "base-2.snap";
                     switch (kind) {
                     case 0:
                         f.put("base-2.snap", "short invalid bytes");
                         break;
                     case 1:
                         check(::mkdir(p.c_str(), 0700) == 0, "directory fixture");
                         break;
                     case 2:
                         check(::mkfifo(p.c_str(), 0600) == 0, "FIFO fixture");
                         break;
                     case 3:
                         check(::symlink("CURRENT", p.c_str()) == 0, "symlink fixture");
                         break;
                     case 4:
                         check(::symlink("missing-target", p.c_str()) == 0, "dangling fixture");
                         break;
                     case 5:
                         f.put("collision-source", "unowned hardlink bytes");
                         check(::link((f.path / "collision-source").c_str(), p.c_str()) == 0,
                               "hardlink fixture");
                         break;
                     case 6:
                         check(::symlink("base-2.snap", p.c_str()) == 0, "self-loop fixture");
                         break;
                     case 7: {
                         socket_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
                         check(socket_fd >= 0, "socket fixture");
                         sockaddr_un address{};
                         address.sun_family = AF_UNIX;
                         const auto text = p.string();
                         check(text.size() < sizeof(address.sun_path), "socket path");
                         std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
                         check(::bind(socket_fd, reinterpret_cast<const sockaddr*>(&address),
                                      sizeof(address)) == 0,
                               "bind fixture socket");
                         break;
                     }
                     case 8:
                         f.put("base-2.snap", std::string(10000, 'x'));
                         break;
                     }
                     const auto before = f.inventory();
                     io->candidate_only = true;
                     auto c = find(root);
                     check(c.valid() && c.generation()->value() == 3,
                           "any directory entry occupies generation");
                     check(f.inventory() == before, "collision type/inode/bytes preserved");
                     if (socket_fd >= 0)
                         check(::close(socket_fd) == 0, "close fixture socket");
                 });
    }
    scenario("filesystem case-alias lookup follows actual namespace",
             [](auto& f, auto& io, auto& root) {
                 f.put("BASE-2.snap", "case variant");
                 struct stat info{};
                 const bool aliases = ::lstat((f.path / "base-2.snap").c_str(), &info) == 0;
                 const auto before = f.inventory();
                 io->candidate_only = true;
                 auto c = find(root);
                 check(c.valid() && c.generation()->value() == (aliases ? 3U : 2U),
                       "actual case alias occupancy");
                 check(f.inventory() == before, "case variant preserved");
                 std::cout << "FILESYSTEM case_alias=" << aliases << '\n';
             });
    scenario("unrelated malformed names and temp-only remnants", [](auto& f, auto& io, auto& root) {
        for (auto* name :
             {"base-02.snap", "base-0.snap", "tail-2.aof.extra", "base-18446744073709551616.snap",
              "base--2.snap", "base-+2.snap", ".unrelated",
              "CURRENT.tmp.00000000000000000000000000000000",
              "base-2.snap.tmp.00000000000000000000000000000000", "tail-2.commit.tmp.bad"})
            f.put(name, "ignored bytes");
        const auto before = f.inventory();
        io->candidate_only = true;
        auto c = find(root);
        check(c.valid() && c.generation()->value() == 2, "exact final names only");
        check(f.inventory() == before, "unrelated entries preserved");
    });
}

void budgets_and_exhaustion() {
    for (auto budget : {0U, 4097U, UINT32_MAX}) {
        scenario("pre-entry budget " + std::to_string(budget), [=](auto&, auto& io, auto& root) {
            io->reset();
            io->candidate_only = true;
            auto c = find(root, 1, budget);
            expected_error(c.error(), RootErrorCode::ResourceLimit);
            check(!c.valid() && !c.generation() && !c.recheck_unused() && root.valid() &&
                      io->used == 0,
                  "local rejection poisoned/probed root");
            check(find(root, 1, 1).valid(), "healthy lease after local rejection");
        });
    }
    for (auto budget : {1U, 2U, 4096U}) {
        for (bool free_last : {false, true}) {
            scenario("budget " + std::to_string(budget) + (free_last ? " last free" : " exhausted"),
                     [=](auto& f, auto& io, auto& root) {
                         const auto before = f.inventory();
                         io->candidate_only = true;
                         io->occupied_through = free_last ? budget : budget + 1;
                         auto c = find(root, 1, budget);
                         check(io->artifact_probes == 3ULL * budget + (free_last ? 3U : 0U),
                               "budget probe count");
                         if (free_last)
                             check(c.valid() && c.generation()->value() == budget + 1,
                                   "last-slot success");
                         else {
                             expected_error(c.error(), RootErrorCode::ResourceLimit);
                             terminal(c, root, *io);
                         }
                         check(f.inventory() == before, "budget path wrote files");
                     });
        }
    }
    for (bool sequence : {false, true}) {
        scenario(sequence ? "captured sequence exhaustion" : "selected generation exhaustion",
                 [=](auto&, auto& io, auto& root) {
                     io->candidate_only = true;
                     auto c = find(root, sequence ? 1 : UINT64_MAX, 1, sequence ? UINT64_MAX : 0);
                     expected_error(c.error(), sequence ? RootErrorCode::SequenceExhausted
                                                        : RootErrorCode::GenerationExhausted);
                     check(io->artifact_probes == 0, "exhaustion attempted names");
                     terminal(c, root, *io);
                 });
    }
    for (auto selected : {UINT64_MAX - 1, UINT64_MAX - 2}) {
        scenario("actual generation exhaustion " + std::to_string(selected), [=](auto&, auto& io,
                                                                                 auto& root) {
            io->candidate_only = true;
            io->all_occupied = true;
            auto c = find(root, selected, static_cast<std::uint32_t>(UINT64_MAX - selected));
            expected_error(c.error(), RootErrorCode::GenerationExhausted);
            check(io->artifact_probes == 3 * (UINT64_MAX - selected), "no wrapped probe");
            terminal(c, root, *io);
        });
    }
    scenario("near-max occupied then free max", [](auto& f, auto& io, auto& root) {
        f.put("tail-18446744073709551614.commit", "occupied");
        io->candidate_only = true;
        auto c = find(root, UINT64_MAX - 2, 2, UINT64_MAX - 1);
        check(c.valid() && c.generation()->value() == UINT64_MAX, "max valid candidate");
    });
    scenario("validated generation inputs reject outside search", [](auto&, auto& io, auto& root) {
        io->reset();
        io->candidate_only = true;
        for (const auto* text :
             {"0", "01", "-1", "+1", " 1", "1 ", "1/2", "18446744073709551616", ""}) {
            bool rejected = false;
            try {
                (void)parse_generation(text);
            } catch (const FormatError&) {
                rejected = true;
            }
            check(rejected, "bad generation parse");
        }
        bool rejected = false;
        try {
            (void)gen(0);
        } catch (const FormatError&) {
            rejected = true;
        }
        check(rejected && root.valid() && io->used == 0, "outside type rejection mutated root");
    });
}

void occupancy_errors_and_changes() {
    for (const auto role : {RootRole::Base, RootRole::Tail, RootRole::Watermark}) {
        const auto ordinal =
            static_cast<std::size_t>(role) - static_cast<std::size_t>(RootRole::Base) + 1;
        for (const auto err :
             {EACCES, EIO, EBADF, ENOTDIR, EOVERFLOW, ENOMEM, ELOOP, EINVAL, 999}) {
            scenario("actual probe errno " + std::to_string(err) + " role " +
                         std::to_string(ordinal),
                     [=](auto&, auto& io, auto& root) {
                         io->candidate_only = true;
                         io->answer_at = ordinal;
                         io->answer_errno = err;
                         io->clobber_errno = ENOSPC;
                         auto c = find(root);
                         expected_error(c.error(), RootErrorCode::IoFailure,
                                        RootOperation::NamedStat, role, RootStage::Call, err);
                         terminal(c, root, *io);
                     });
        }
        for (const auto& answer : {std::pair{1, 0}, {-2, EIO}, {-1, 0}, {-1, -7}}) {
            scenario("invalid probe result " + std::to_string(answer.first) + "/" +
                         std::to_string(answer.second) + " role " + std::to_string(ordinal),
                     [=](auto&, auto& io, auto& root) {
                         io->candidate_only = true;
                         io->answer_at = ordinal;
                         io->answer_result = answer.first;
                         io->answer_errno = answer.second;
                         auto c = find(root);
                         expected_error(c.error(), RootErrorCode::IoFailure,
                                        RootOperation::NamedStat, role, RootStage::Call, EIO);
                         terminal(c, root, *io);
                     });
        }
        for (const auto err : {EINTR, ENOENT}) {
            scenario("actual retry/absence " + std::to_string(err) + " role " +
                         std::to_string(ordinal),
                     [=](auto&, auto& io, auto& root) {
                         io->candidate_only = true;
                         io->answer_at = ordinal;
                         io->answer_errno = err;
                         io->clobber_errno = EACCES;
                         auto c = find(root);
                         check(c.valid() && c.generation()->value() == 2 &&
                                   io->artifact_probes == (err == EINTR ? 7U : 6U),
                               "actual EINTR retry/ENOENT absence");
                         io->dump("actual retry/absence");
                     });
        }
        scenario("confirmation collision role " + std::to_string(ordinal),
                 [=](auto& f, auto& io, auto& root) {
                     io->fixture = &f;
                     io->create_at_probe = 4;
                     io->create_name = role == RootRole::Base   ? "base-2.snap"
                                       : role == RootRole::Tail ? "tail-2.aof"
                                                                : "tail-2.commit";
                     io->candidate_only = true;
                     auto c = find(root);
                     expected_error(c.error(), RootErrorCode::CandidateOccupied,
                                    RootOperation::NamedStat, role);
                     check(io->artifact_probes == 6, "confirmation must inspect all3");
                     terminal(c, root, *io);
                 });
        scenario("recheck occupancy role " + std::to_string(ordinal),
                 [=](auto& f, auto& io, auto& root) {
                     io->candidate_only = true;
                     auto c = find(root);
                     f.put(role == RootRole::Base   ? "base-2.snap"
                           : role == RootRole::Tail ? "tail-2.aof"
                                                    : "tail-2.commit",
                           "unowned new entry");
                     const auto before = f.inventory();
                     io->reset();
                     check(!c.recheck_unused() && io->artifact_probes == 3, "recheck probes all3");
                     expected_error(c.error(), RootErrorCode::CandidateOccupied,
                                    RootOperation::NamedStat, role);
                     terminal(c, root, *io);
                     check(f.inventory() == before, "recheck deleted collision");
                 });
    }
    for (int outcome = 0; outcome < 3; ++outcome) {
        scenario("repeated actual EINTR outcome " + std::to_string(outcome), [=](auto& f, auto& io,
                                                                                 auto& root) {
            if (outcome == 1)
                f.put("base-2.snap", "occupied after retries");
            io->interrupts_remaining = 3;
            io->following_errno = outcome == 2 ? EIO : 0;
            io->candidate_only = true;
            auto c = find(root);
            check(io->interrupts_remaining == 0, "repeated EINTR unreached");
            if (outcome == 2) {
                expected_error(c.error(), RootErrorCode::IoFailure, RootOperation::NamedStat,
                               RootRole::Base, RootStage::Call, EIO);
                terminal(c, root, *io);
            } else
                check(c.valid() && c.generation()->value() == (outcome == 1 ? 3U : 2U),
                      "retry eventual classification");
        });
    }
    scenario("later error not hidden by earlier occupancy", [](auto& f, auto& io, auto& root) {
        f.put("base-2.snap", "occupied");
        io->answer_at = 3;
        io->answer_errno = EIO;
        io->candidate_only = true;
        auto c = find(root);
        check(io->artifact_probes == 3, "later role not reached");
        expected_error(c.error(), RootErrorCode::IoFailure, RootOperation::NamedStat,
                       RootRole::Watermark, RootStage::Call, EIO);
        terminal(c, root, *io);
    });
    scenario("entry appearing before initial probe skips", [](auto& f, auto& io, auto& root) {
        io->fixture = &f;
        io->create_at_probe = 1;
        io->candidate_only = true;
        auto c = find(root);
        check(c.valid() && c.generation()->value() == 3, "initial occupancy is skip");
    });
    for (auto role : {RootRole::Root, RootRole::LockFile}) {
        for (auto edit : {Adapter::Edit::Inode, Adapter::Edit::Device, Adapter::Edit::Type,
                          Adapter::Edit::Links, Adapter::Edit::Size}) {
            if (role == RootRole::Root &&
                (edit == Adapter::Edit::Links || edit == Adapter::Edit::Size))
                continue;
            scenario("held root/LOCK identity " + std::to_string(static_cast<int>(role)) + "/" +
                         std::to_string(static_cast<int>(edit)),
                     [=](auto&, auto& io, auto& root) {
                         io->edit_role = role;
                         io->edit = edit;
                         io->candidate_only = true;
                         auto c = find(root);
                         check(io->artifact_probes == 0, "identity failed before probes");
                         terminal(c, root, *io);
                     });
        }
    }
    for (auto after : {3U, 6U}) {
        scenario("root identity postcheck after probe " + std::to_string(after),
                 [=](auto&, auto& io, auto& root) {
                     io->edit_root_after_probe = after;
                     io->candidate_only = true;
                     auto c = find(root);
                     check(io->artifact_probes == after, "postcheck location");
                     expected_error(c.error(), RootErrorCode::IdentityChanged, RootOperation::Stat,
                                    RootRole::Root);
                     terminal(c, root, *io);
                 });
    }
    for (int kind = 0; kind < 4; ++kind) {
        scenario("named LOCK mutation " + std::to_string(kind), [=](auto& f, auto& io, auto& root) {
            check(::rename((f.path / "LOCK").c_str(), (f.path / "original-lock").c_str()) == 0,
                  "save LOCK");
            if (kind == 0)
                f.put("LOCK", "");
            if (kind == 1)
                check(::symlink("original-lock", (f.path / "LOCK").c_str()) == 0, "LOCK symlink");
            if (kind == 2)
                check(::link((f.path / "original-lock").c_str(), (f.path / "LOCK").c_str()) == 0,
                      "LOCK hardlink");
            io->candidate_only = true;
            auto c = find(root);
            terminal(c, root, *io);
            if (kind != 3)
                f.remove("LOCK");
            check(::rename((f.path / "original-lock").c_str(), (f.path / "LOCK").c_str()) == 0,
                  "restore harness LOCK");
        });
    }
    scenario("held directory remains authority after pathname replacement", [](auto& f, auto& io,
                                                                               auto& root) {
        const auto held = std::filesystem::path(f.path.string() + "-held");
        check(::rename(f.path.c_str(), held.c_str()) == 0 && ::mkdir(f.path.c_str(), 0700) == 0,
              "replace root path");
        io->candidate_only = true;
        auto c = find(root);
        const bool ok = c.valid() && c.recheck_unused();
        check(::rmdir(f.path.c_str()) == 0 && ::rename(held.c_str(), f.path.c_str()) == 0,
              "restore root path");
        check(ok, "held directory semantics changed");
    });
}

void contender(const Fixture& f, const Adapter& io, bool locked) {
    int pipefd[2];
    check(::pipe(pipefd) == 0, "child pipe");
    const auto pid = ::fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        (void)::close(pipefd[0]);
        for (const auto& d : io.descriptors)
            if (d.fd >= 0)
                (void)::close(d.fd);
        auto own = RootLease::open_existing(f.path);
        const bool ok = locked ? !own.valid() && own.error().code == RootErrorCode::LockUnavailable
                               : own.valid();
        const char answer = ok ? '!' : '?';
        if (::write(pipefd[1], &answer, 1) != 1)
            ::_exit(2);
        ::_exit(ok ? 0 : 1);
    }
    (void)::close(pipefd[1]);
    pollfd ready{pipefd[0], POLLIN, 0};
    char answer = 0;
    const bool notified = ::poll(&ready, 1, 5000) > 0 && ::read(pipefd[0], &answer, 1) == 1;
    (void)::close(pipefd[0]);
    if (!notified)
        (void)::kill(pid, SIGKILL);
    int status = 0;
    check(::waitpid(pid, &status, 0) == pid, "wait child");
    check(notified && answer == '!' && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "independent child contention");
}
void ownership_and_allocations() {
    scenario("default/moved/closed/failed root states", [](auto&, auto& io, auto& root) {
        GenerationCandidate empty;
        check(!empty.valid() && !empty.generation() && !empty.recheck_unused(), "default state");
        RootLease missing;
        auto no_root = find(missing);
        expected_error(no_root.error(), RootErrorCode::ClosedOrFailed);
        auto moved = std::move(root);
        auto moved_from = find(root);
        expected_error(moved_from.error(), RootErrorCode::ClosedOrFailed);
        check(moved.close_checked(), "healthy root close");
        io->reset();
        auto closed = find(moved);
        expected_error(closed.error(), RootErrorCode::ClosedOrFailed);
        check(io->used == 0, "closed root syscall");
    });
    scenario("moves/self-move preserve one observation lease", [](auto&, auto& io, auto& root) {
        io->candidate_only = true;
        auto a = find(root);
        const auto observed = *a.generation();
        auto b = std::move(a);
        check(!a.valid() && !a.generation() && !a.recheck_unused() && b.valid() &&
                  *b.generation() == observed,
              "move construct");
        auto* self = &b;
        b = std::move(*self);
        check(b.valid() && b.recheck_unused(), "self move");
        auto c = find(root, 2);
        c = std::move(b);
        check(c.valid() && c.generation()->value() == 2 && !b.valid(), "move assign");
        check(io->live() == 2, "candidate opened artifact descriptor");
    });
    scenario("root/pins/staging/candidate share lock lifetime", [](auto& f, auto& io, auto& root) {
        auto pin = root.pin_current();
        auto staged = StagedFile::create_data(root, ArtifactRole::Tail, gen(2));
        check(staged.valid(), "staging sibling");
        auto c = find(root);
        root = RootLease{};
        check(c.recheck_unused() && pin.recheck() && staged.recheck(),
              "root destroyed with dependents");
        contender(f, *io, true);
        staged = StagedFile{};
        contender(f, *io, true);
        pin = PinnedFile{};
        contender(f, *io, true);
        io->candidate_only = true;
        c = GenerationCandidate{};
        check(io->live() == 0, "candidate final release");
        contender(f, *io, false);
        check(io->closes == io->opens, "final close accounting");
    });
    scenario("explicit root close with candidate is terminal DependentsAlive",
             [](auto& f, auto& io, auto& root) {
                 io->candidate_only = true;
                 auto c = find(root);
                 check(!root.close_checked(), "root close with dependent succeeded");
                 expected_error(c.error(), RootErrorCode::DependentsAlive, RootOperation::Close);
                 terminal(c, root, *io);
                 contender(f, *io, true);
             });
    scenario("staging composition healthy exact-owned cleanup", [](auto& f, auto& io, auto& root) {
        const auto before = f.inventory();
        io->candidate_only = true;
        auto c = find(root);
        io->candidate_only = false;
        for (auto role : {ArtifactRole::Base, ArtifactRole::Tail, ArtifactRole::Watermark}) {
            auto s = StagedFile::create_data(root, role, *c.generation());
            std::array<std::byte, 3> bytes{std::byte{0}, std::byte{255}, std::byte{128}}, output{};
            check(s.valid() && s.append(bytes) && s.read_exact(0, output) && bytes == output &&
                      s.seal_file() && s.discard_checked(),
                  "unchanged staging API composition");
            io->candidate_only = true;
            check(c.recheck_unused(), "temporary does not occupy final names");
            io->candidate_only = false;
        }
        check(f.inventory() == before, "healthy cleanup exact owned temps only");
    });
    for (int completed = 0; completed < 3; ++completed) {
        scenario(
            "partial temporary setup failure after roles " + std::to_string(completed),
            [=](auto& f, auto& io, auto& root) {
                auto c = find(root);
                std::array<StagedFile, 3> staged;
                const std::array<ArtifactRole, 3> roles{ArtifactRole::Base, ArtifactRole::Tail,
                                                        ArtifactRole::Watermark};
                for (int i = 0; i < completed; ++i) {
                    staged[static_cast<std::size_t>(i)] = StagedFile::create_data(
                        root, roles[static_cast<std::size_t>(i)], *c.generation());
                    check(staged[static_cast<std::size_t>(i)].valid(), "earlier staging create");
                }
                const auto before = f.inventory();
                io->reset();
                io->fail_create = true; // Actual existing staging create fails, after prior roles.
                auto failed = StagedFile::create_data(
                    root, roles[static_cast<std::size_t>(completed)], *c.generation());
                check(!failed.valid() && io->reached == 1 && !c.valid(),
                      "partial setup shared failure");
                const auto unlinks = io->unlinks;
                io->target = 0;
                for (auto& s : staged)
                    s = StagedFile{};
                check(io->unlinks == unlinks && f.inventory() == before,
                      "failed setup retains linked temps");
                terminal(c, root, *io);
            });
    }
    scenario("search failure blocks staging writes/sync/cleanup",
             [](auto& f, auto& io, auto& root) {
                 auto pin = root.pin_current();
                 auto staged = StagedFile::create_data(root, ArtifactRole::Base, gen(2));
                 check(staged.valid(), "staging setup");
                 const auto before = f.inventory();
                 io->candidate_only = true;
                 io->answer_at = 1;
                 io->answer_errno = EIO;
                 auto c = find(root);
                 const auto first = c.error();
                 check(!pin.valid() && !staged.valid(), "shared root failure propagation");
                 const auto unlinks = io->unlinks;
                 check(!staged.append({}) && !staged.seal_file() && !staged.discard_checked(),
                       "failed sibling operations");
                 check(staged.error() == first && io->unlinks == unlinks && f.inventory() == before,
                       "first error/retention");
                 terminal(c, root, *io);
             });
    scenario("cross-root failure isolation and move", [](auto&, auto& io, auto& root) {
        Fixture other;
        other.files();
        const auto before = other.inventory();
        auto other_io = std::make_shared<Adapter>();
        {
            auto other_root = RootLease::open_existing(other.path, other_io);
            auto incoming = find(other_root);
            auto target = find(root);
            io->edit_named_lock = true;
            check(!target.recheck_unused(), "failed target");
            const auto first = target.error();
            target = std::move(incoming);
            check(!target.valid() && target.error() == first && !incoming.valid() &&
                      other_root.valid(),
                  "move cannot restore health/poison other root");
            const auto used = other_io->used;
            check(!target.recheck_unused() && other_io->used == used,
                  "failed target probes incoming root");
            check(find(other_root).valid(), "other root search");
        }
        check(other_io->live() == 0 && other.inventory() == before,
              "cross-root lifetime/preservation");
    });
    for (auto role : {RootRole::LockFile, RootRole::Root}) {
        for (auto stage : {RootStage::Before, RootStage::Call, RootStage::After}) {
            for (auto err : {EIO, EINTR}) {
                scenario("last-reference close " + std::to_string(static_cast<int>(role)) + "/" +
                             std::to_string(static_cast<int>(stage)) + "/" + std::to_string(err),
                         [=](auto& f, auto& io, auto& root) {
                             Fixture other;
                             auto other_io = std::make_shared<Adapter>();
                             auto other_root = RootLease::open_existing(other.path, other_io);
                             auto incoming = find(other_root);
                             auto target = find(root);
                             root = RootLease{};
                             io->close_fault = true;
                             io->close_role = role;
                             io->close_stage = stage;
                             io->close_errno = err;
                             io->reuse_close = stage == RootStage::Call;
                             const auto closes = io->closes;
                             target = std::move(incoming);
                             expected_error(target.error(), RootErrorCode::IoFailure,
                                            RootOperation::Close, role, stage, err);
                             check(!target.valid() && other_root.valid() &&
                                       io->closes == closes + 2 && io->close_hits == 1 &&
                                       io->live() == 0,
                                   "last-reference close failure captured/once-only");
                             target = GenerationCandidate{};
                             check(io->closes == closes + 2, "close retried on later disposal");
                             if (io->replacement >= 0) {
                                 check(::fcntl(io->replacement, F_GETFD) >= 0 &&
                                           ::close(io->replacement) == 0,
                                       "reused descriptor preserved");
                                 io->replacement = -1;
                             }
                             contender(f, *io, false);
                         });
            }
        }
    }
    scenario("destructor last-reference close failure still attempts root once", [](auto&, auto& io,
                                                                                    auto& root) {
        {
            auto c = find(root);
            root = RootLease{};
            io->close_fault = true;
            io->close_role = RootRole::LockFile;
            io->close_stage = RootStage::Before;
        }
        check(io->live() == 0 && io->closes == 2 && io->close_hits == 1, "destructor close path");
        check(io->trace[io->used - 1].event.operation == RootOperation::Close &&
                  io->trace[io->used - 1].event.role == RootRole::Root,
              "LOCK then root teardown");
    });
    scenario("prior shared error wins over last-reference close failure",
             [](auto&, auto& io, auto& root) {
                 io->candidate_only = true;
                 io->answer_at = 1;
                 io->answer_errno = EACCES;
                 auto c = find(root);
                 const auto first = c.error();
                 root = RootLease{};
                 io->close_fault = true;
                 io->close_role = RootRole::LockFile;
                 io->close_errno = EIO;
                 c = GenerationCandidate{};
                 check(c.error() == first && io->close_hits == 1 && io->live() == 0,
                       "teardown replaced first error");
             });
    scenario(
        "process termination releases observation without persistent reservation",
        [](auto& f, auto& io, auto& root) {
            const auto before = f.inventory();
            root = RootLease{};
            int pipefd[2];
            check(::pipe(pipefd) == 0, "crash child pipe");
            const auto pid = ::fork();
            check(pid >= 0, "crash child fork");
            if (pid == 0) {
                (void)::close(pipefd[0]);
                auto own = RootLease::open_existing(f.path);
                auto c = find(own);
                const char answer = c.valid() ? '!' : '?';
                if (::write(pipefd[1], &answer, 1) != 1)
                    ::_exit(2);
                (void)::raise(SIGSTOP);
                ::_exit(3);
            }
            (void)::close(pipefd[1]);
            pollfd ready{pipefd[0], POLLIN, 0};
            char answer = 0;
            const bool notified = ::poll(&ready, 1, 5000) > 0 && ::read(pipefd[0], &answer, 1) == 1;
            (void)::close(pipefd[0]);
            if (!notified) {
                (void)::kill(pid, SIGKILL);
                int status = 0;
                (void)::waitpid(pid, &status, 0);
                check(false, "crash child notification");
            }
            int status = 0;
            check(::waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status) && answer == '!',
                  "crash boundary stop");
            contender(f, *io, true);
            check(::kill(pid, SIGKILL) == 0 && ::waitpid(pid, &status, 0) == pid &&
                      WIFSIGNALED(status),
                  "owned child termination");
            contender(f, *io, false);
            auto next = RootLease::open_existing(f.path);
            auto again = find(next);
            check(again.valid() && again.generation()->value() == 2 && f.inventory() == before,
                  "crash did not retain/reserve/mutate candidate");
        });
    scenario("zero allocations in full candidate lifecycle", [](auto&, auto& io, auto& root) {
        io->candidate_only = true;
        const auto allocations = guarded_allocations;
        allocation_budget = 0;
        auto a = find(root);
        const bool first = a.valid() && a.recheck_unused();
        auto b = std::move(a);
        auto c = find(root, 2);
        c = std::move(b);
        const bool second = c.valid() && c.recheck_unused();
        root = RootLease{};
        c = GenerationCandidate{};
        allocation_budget = -1;
        check(first && second && guarded_allocations == allocations && io->live() == 0,
              "candidate path allocated");
        std::cout << "ALLOCATION candidate lifecycle zero reached allocation sites\n";
    });
    scenario("reached RootState allocation failure propagates", [](auto& f, auto& io, auto& root) {
        root = RootLease{};
        const auto rejects = allocation_rejections;
        allocation_budget = 0;
        auto failed_root = RootLease::open_existing(f.path, io);
        auto c = find(failed_root);
        allocation_budget = -1;
        check(allocation_rejections == rejects + 1 && !c.valid() &&
                  c.error().code == RootErrorCode::ResourceLimit && io->live() == 0,
              "reached state allocation failure");
        std::cout << "ALLOCATION reached RootState failure=1\n";
    });
}

enum class Action { Search, Skip, Recheck, Release };
GenerationCandidate prepare(Action action, RootLease& root) {
    return action == Action::Recheck || action == Action::Release ? find(root)
                                                                  : GenerationCandidate{};
}
bool invoke(Action action, GenerationCandidate& c, RootLease& root) {
    switch (action) {
    case Action::Search:
    case Action::Skip:
        c = find(root);
        return c.valid();
    case Action::Recheck:
        return c.recheck_unused();
    case Action::Release:
        c = GenerationCandidate{};
        return c.error().code == RootErrorCode::ClosedOrFailed;
    }
    return false;
}
void boundary_matrix() {
    for (auto action : {Action::Search, Action::Skip, Action::Recheck, Action::Release}) {
        std::array<Adapter::Trace, 512> expected{};
        std::size_t boundaries = 0;
        scenario("discover action " + std::to_string(static_cast<int>(action)),
                 [&](auto& f, auto& io, auto& root) {
                     if (action == Action::Skip)
                         f.put("base-2.snap", "occupied");
                     io->candidate_only = true;
                     auto c = prepare(action, root);
                     if (action == Action::Release)
                         root = RootLease{};
                     io->reset();
                     check(invoke(action, c, root), "healthy discovery");
                     boundaries = io->used;
                     check(boundaries <= expected.size(), "boundary capacity");
                     std::copy_n(io->trace.begin(), boundaries, expected.begin());
                     io->dump("healthy discovered action");
                 });
        for (std::size_t point = 1; point <= boundaries; ++point) {
            scenario("reached action " + std::to_string(static_cast<int>(action)) + " boundary " +
                         std::to_string(point),
                     [&](auto& f, auto& io, auto& root) {
                         if (action == Action::Skip)
                             f.put("base-2.snap", "occupied");
                         const auto before = f.inventory();
                         io->candidate_only = true;
                         auto c = prepare(action, root);
                         if (action == Action::Release)
                             root = RootLease{};
                         io->reset();
                         io->target = point;
                         check(!invoke(action, c, root) && io->reached == 1,
                               "injection unreached or accepted");
                         const auto& planned = expected[point - 1].event;
                         const auto& actual = io->trace[point - 1].event;
                         check(actual.operation == planned.operation &&
                                   actual.role == planned.role && actual.stage == planned.stage,
                               "exact discovered boundary mismatch");
                         expected_error(c.error(), RootErrorCode::IoFailure, planned.operation,
                                        planned.role, planned.stage, EIO);
                         if (action != Action::Release)
                             terminal(c, root, *io);
                         else
                             check(io->live() == 0 && io->closes == 2, "release closes both once");
                         check(f.inventory() == before, "injection changed files");
                         ++injections;
                         io->dump("reached boundary injection");
                     });
        }
    }
    // Explicitly distinguish hook EINTR from actual EINTR and error precedence.
    for (auto stage : {RootStage::Before, RootStage::After}) {
        scenario("observer EINTR is terminal " + std::to_string(static_cast<int>(stage)),
                 [=](auto&, auto& io, auto& root) {
                     io->candidate_only = true;
                     io->reset();
                     auto c = find(root);
                     std::size_t point = 0;
                     for (std::size_t i = 0; i < io->used; ++i)
                         if (io->trace[i].event.operation == RootOperation::NamedStat &&
                             io->trace[i].event.role == RootRole::Base &&
                             io->trace[i].event.stage == stage) {
                             point = i + 1;
                             break;
                         }
                     check(point != 0, "hook target discovery");
                     io->reset();
                     io->target = point;
                     io->fault_errno = EINTR;
                     auto failed = find(root);
                     expected_error(failed.error(), RootErrorCode::IoFailure,
                                    RootOperation::NamedStat, RootRole::Base, stage, EINTR);
                     check(io->reached == 1 &&
                               io->artifact_probes == (stage == RootStage::Before ? 0U : 1U),
                           "observer EINTR retried");
                     ++injections;
                     terminal(failed, root, *io);
                 });
    }
    for (auto actual_errno : {EIO, EINTR}) {
        scenario("actual errno / After rejection precedence " + std::to_string(actual_errno),
                 [=](auto&, auto& io, auto& root) {
                     io->candidate_only = true;
                     io->reset();
                     auto c = find(root);
                     std::size_t point = 0;
                     for (std::size_t i = 0; i < io->used; ++i)
                         if (io->trace[i].event.operation == RootOperation::NamedStat &&
                             io->trace[i].event.role == RootRole::Base &&
                             io->trace[i].event.stage == RootStage::After) {
                             point = i + 1;
                             break;
                         }
                     check(point != 0, "precedence target");
                     io->reset();
                     io->target = point;
                     io->fault_errno = EACCES;
                     io->answer_at = 1;
                     io->answer_errno = actual_errno;
                     auto failed = find(root);
                     expected_error(failed.error(), RootErrorCode::IoFailure,
                                    RootOperation::NamedStat, RootRole::Base,
                                    actual_errno == EINTR ? RootStage::After : RootStage::Call,
                                    actual_errno == EINTR ? EACCES : EIO);
                     check(io->reached == 1 && io->artifact_probes == 1, "After rejection retry");
                     ++injections;
                     terminal(failed, root, *io);
                 });
    }
}
std::array<bool, 512> descriptor_inventory() {
    std::array<bool, 512> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = ::fcntl(static_cast<int>(i), F_GETFD) >= 0;
    return result;
}
} // namespace
int main() {
    try {
        const auto descriptors = descriptor_inventory();
        positives_and_names();
        budgets_and_exhaustion();
        occupancy_errors_and_changes();
        ownership_and_allocations();
        boundary_matrix();
        check(descriptor_inventory() == descriptors, "process descriptor inventory changed");
        std::cout << "PASS V2 generation: " << scenarios << " executed scenarios; " << injections
                  << " reached boundary injections; no reservation/initialization/publication; "
                     "owned fixtures only\n";
    } catch (const std::exception& e) {
        allocation_budget = -1;
        std::cerr << "FAIL after " << scenarios << " scenarios: " << e.what() << '\n';
        return 1;
    }
}
