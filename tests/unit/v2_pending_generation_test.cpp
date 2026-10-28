#include "redisdb/persistence/V2PendingGeneration.hpp"
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
        std::strcpy(name.data(), "/tmp/redisdb-v2-pending-XXXXXX");
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

static_assert(std::is_final_v<PendingGeneration>);
static_assert(!std::is_copy_constructible_v<PendingGeneration>);
static_assert(!std::is_copy_assignable_v<PendingGeneration>);
static_assert(std::is_nothrow_move_constructible_v<PendingGeneration>);
static_assert(std::is_nothrow_move_assignable_v<PendingGeneration>);
static_assert(std::is_nothrow_destructible_v<PendingGeneration>);
static_assert(sizeof(PendingGeneration) < 2048);
static_assert(
    std::is_same_v<decltype(std::declval<const PendingGeneration&>().identity(ArtifactRole::Base)),
                   const FileIdentity*>);
template <class T>
concept ContentOrPublication =
    requires(T& p) { p.append(ByteView{}); } || requires(T& p) { p.write(ByteView{}); } ||
    requires(T& p) { p.seal_file(); } || requires(T& p) { p.publish(); } ||
    requires(T& p) { p.install(); } || requires(T& p) { p.activate(); } ||
    requires(T& p) { p.fd(); } || requires(T& p) { p.filename(); } ||
    requires(T& p) { p.staged(ArtifactRole::Base); } ||
    requires(T& p, RootLease& r) { T::first_for_explicit_initialization(r); };
static_assert(!ContentOrPublication<PendingGeneration>);
static_assert(static_cast<int>(RootErrorCode::WrongState) == 15);
static_assert(static_cast<int>(RootErrorCode::CandidateOccupied) == 18);
static_assert(static_cast<int>(RootOperation::UnlinkAt) == 12);
static_assert(static_cast<int>(RootRole::StagedWatermark) == 8);
constexpr std::array<ArtifactRole, 3> Roles{ArtifactRole::Base, ArtifactRole::Tail,
                                            ArtifactRole::Watermark};
constexpr std::array<RootRole, 3> StagedRoles{RootRole::StagedBase, RootRole::StagedTail,
                                              RootRole::StagedWatermark};
constexpr std::array<const char*, 3> FinalNames{"base-2.snap", "tail-2.aof", "tail-2.commit"};
bool final_role(RootRole r) { return r >= RootRole::Base && r <= RootRole::Watermark; }
bool staged_role(RootRole r) { return r >= RootRole::StagedBase && r <= RootRole::StagedWatermark; }

// Fixed-capacity, allocation-free hooks. Actual Call events are independent of
// Before/After observers. All direct mutations here target this harness's root.
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
    struct Temp {
        std::array<char, 70> name{};
        RootRole role = RootRole::Root;
        int fd = -1;
        bool known = false, removed = false;
    };
    std::array<Descriptor, 32> descriptors{};
    std::array<Trace, 2048> trace{};
    std::array<Temp, 256> temps{};
    std::size_t used = 0, target = 0, reached = 0, temp_count = 0;
    std::size_t opens = 0, closes = 0, creates = 0, randoms = 0, probes = 0, unlinks = 0;
    std::size_t reads = 0, writes = 0, syncs = 0, unlinks_after_fault = 0;
    bool capture = true, fault_seen = false, flags_ok = true, tracking_ok = true;
    bool forbid_reopen = false, forbid_content = true, fixed_token = false, real_entropy = false;
    RootRole active = RootRole::Root;
    int fault_errno = EIO, clobber_errno = 0;
    std::size_t answer_at = 0, interrupts = 0;
    int answer_result = -1, answer_errno = EIO, following_errno = 0;
    std::uint64_t occupied_through = 0;
    bool all_occupied = false;
    std::size_t occupy_probe = 0, occupy_after_create = 0;
    const char* occupy_name = "base-2.snap";
    RootRole collision_role = static_cast<RootRole>(99);
    unsigned collisions = 0;
    RootRole entropy_role = static_cast<RootRole>(99),
             create_error_role = static_cast<RootRole>(99);
    int entropy_errno = EIO, entropy_result = -1, create_errno = ENOSPC, create_result = -1;
    bool ambiguous_create = false, ambiguous_unlink = false;
    int unlink_errno = 0, invalid_stat_result = 0;
    RootRole edit_role = static_cast<RootRole>(99);
    enum class Edit { Inode, Device, Type, Links, Size, Negative, Huge } edit = Edit::Inode;
    bool edit_named = false;
    bool close_fault = false, reuse_close = false, invalid_close = false;
    RootRole close_role = RootRole::LockFile;
    RootStage close_stage = RootStage::Call;
    int close_errno = EIO, replacement = -1;
    std::size_t close_hits = 0;
    int pause_fd = -1;
    unsigned pause_create = 99;
    bool pause_before = false;
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
        if (capture) {
            if (used >= trace.size())
                std::abort();
            auto& t = trace[used];
            t.event = e;
            t.fd = fd;
            if (name) {
                const auto n = std::strlen(name);
                if (n >= t.name.size())
                    std::abort();
                std::memcpy(t.name.data(), name, n + 1);
            }
        }
        ++used;
        if (target && used == target) {
            ++reached;
            fault_seen = true;
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
        used = target = reached = probes = unlinks_after_fault = 0;
        fault_seen = false;
        capture = true;
        trace.fill({});
    }
    void make_final(int dir, const char* name) noexcept {
        const auto fd =
            ::openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0 || ::close(fd) != 0)
            std::abort();
    }
    void modify(struct stat* s) const noexcept {
        switch (edit) {
        case Edit::Inode:
            ++s->st_ino;
            break;
        case Edit::Device:
            ++s->st_dev;
            break;
        case Edit::Type:
            s->st_mode = active == RootRole::Root ? S_IFREG : S_IFDIR;
            break;
        case Edit::Links:
            s->st_nlink = 2;
            break;
        case Edit::Size:
            ++s->st_size;
            break;
        case Edit::Negative:
            s->st_size = -1;
            break;
        case Edit::Huge:
            s->st_size = std::numeric_limits<off_t>::max();
            break;
        }
    }
    int observe(const RootIoEvent& e) noexcept override {
        active = e.role;
        const bool hit = record(e);
        if (pause_fd >= 0 && e.operation == RootOperation::CreateAt &&
            ((pause_before && e.stage == RootStage::Before) ||
             (!pause_before && e.stage == RootStage::After && creates == pause_create))) {
            const char ready = '!';
            if (::write(pause_fd, &ready, 1) != 1)
                ::_exit(91);
            (void)::raise(SIGSTOP);
            ::_exit(92);
        }
        if (close_fault && e.operation == RootOperation::Close && e.role == close_role &&
            e.stage == close_stage) {
            ++close_hits;
            return close_errno;
        }
        if (e.stage == RootStage::After && clobber_errno)
            errno = clobber_errno;
        return hit ? fault_errno : 0;
    }
    int open(const char* p, int flags) noexcept override {
        if (forbid_reopen)
            std::abort();
        if (!call(RootOperation::Open, active))
            return -1;
        const auto fd = RootIo::open(p, flags);
        adopt(fd);
        return fd;
    }
    int open_at(int dir, const char* name, int flags) noexcept override {
        if (forbid_reopen)
            std::abort();
        if (!call(RootOperation::OpenAt, active, dir, name))
            return -1;
        const auto fd = RootIo::open_at(dir, name, flags);
        adopt(fd);
        return fd;
    }
    int lock(int fd, int flags) noexcept override {
        if (forbid_reopen)
            std::abort();
        if (!call(RootOperation::Lock, active, fd))
            return -1;
        return RootIo::lock(fd, flags);
    }
    int stat(int fd, struct stat* info) noexcept override {
        active = role_of(fd);
        if (!call(RootOperation::Stat, active, fd))
            return -1;
        if (invalid_stat_result)
            return invalid_stat_result;
        const auto r = RootIo::stat(fd, info);
        if (r == 0 && active == edit_role && !edit_named)
            modify(info);
        return r;
    }
    int named_stat(int dir, const char* name, struct stat* info, int flags) noexcept override {
        flags_ok = flags_ok && flags == AT_SYMLINK_NOFOLLOW && std::strchr(name, '/') == nullptr;
        if (final_role(active)) {
            ++probes;
            if (occupy_probe == probes)
                make_final(dir, name);
        }
        if (!call(RootOperation::NamedStat, active, dir, name))
            return -1;
        if (final_role(active)) {
            if (interrupts) {
                --interrupts;
                errno = EINTR;
                return -1;
            }
            if (following_errno) {
                errno = std::exchange(following_errno, 0);
                return -1;
            }
            if (answer_at == probes) {
                errno = answer_errno;
                return answer_result;
            }
            if (all_occupied || occupied_through) {
                std::uint64_t g = 0;
                const char* p = name + 5;
                while (*p >= '0' && *p <= '9')
                    g = g * 10 + static_cast<unsigned>(*p++ - '0');
                if (all_occupied || g <= occupied_through)
                    return 0;
            }
        }
        const auto r = RootIo::named_stat(dir, name, info, flags);
        if (r == 0 && edit_named && active == edit_role)
            modify(info);
        return r;
    }
    int random_128(std::array<std::byte, 16>& token) noexcept override {
        ++randoms;
        if (!call(RootOperation::Random128, active))
            return -1;
        if (active == entropy_role) {
            errno = entropy_errno;
            return entropy_result;
        }
        if (real_entropy)
            return RootIo::random_128(token);
        token.fill(std::byte{0});
        if (!fixed_token) {
            auto n = randoms;
            for (std::size_t i = 0; i < 8; ++i) {
                token[token.size() - 1 - i] = static_cast<std::byte>(n & 255U);
                n >>= 8;
            }
        }
        return 0;
    }
    int create_at(int dir, const char* name, int flags, mode_t mode) noexcept override {
        ++creates;
        if (!staged_role(active) ||
            flags != (O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) ||
            mode != 0600 || std::strlen(name) > 69 || std::strchr(name, '/') ||
            !std::strstr(name, ".tmp."))
            std::abort();
        if (!call(RootOperation::CreateAt, active, dir, name))
            return -1;
        if (active == collision_role && collisions) {
            --collisions;
            errno = EEXIST;
            return -1;
        }
        if (active == create_error_role) {
            errno = create_errno;
            return create_result;
        }
        const auto fd = RootIo::create_at(dir, name, flags, mode);
        if (fd >= 0) {
            if (temp_count == temps.size())
                std::abort();
            auto& t = temps[temp_count++];
            std::strcpy(t.name.data(), name);
            t.role = active;
            t.fd = fd;
            if (ambiguous_create) {
                ambiguous_create = false;
                if (::close(fd) != 0)
                    std::abort();
                errno = EINTR;
                return -1; // Unknown temp remains; never returned as owned FD.
            }
            t.known = true;
            adopt(fd);
            if (occupy_after_create == temp_count)
                make_final(dir, occupy_name);
        }
        return fd;
    }
    ssize_t read(int fd, void* p, std::size_t n, off_t off) noexcept override {
        if (forbid_content)
            std::abort();
        ++reads;
        return RootIo::read(fd, p, n, off);
    }
    ssize_t write_at(int fd, const void* p, std::size_t n, off_t off) noexcept override {
        if (forbid_content)
            std::abort();
        ++writes;
        return RootIo::write_at(fd, p, n, off);
    }
    int sync_file(int fd) noexcept override {
        if (forbid_content)
            std::abort();
        ++syncs;
        return RootIo::sync_file(fd);
    }
    int unlink_at(int dir, const char* name) noexcept override {
        ++unlinks;
        if (fault_seen)
            ++unlinks_after_fault;
        bool owned = false;
        for (const auto& t : temps)
            if (t.known && !t.removed && std::strcmp(t.name.data(), name) == 0)
                owned = true;
        if (!owned || !std::strstr(name, ".tmp."))
            std::abort();
        if (!call(RootOperation::UnlinkAt, active, dir, name))
            return -1;
        if (unlink_errno) {
            errno = unlink_errno;
            return -1;
        }
        const auto r = RootIo::unlink_at(dir, name);
        if (r == 0)
            for (auto& t : temps)
                if (std::strcmp(t.name.data(), name) == 0)
                    t.removed = true;
        if (ambiguous_unlink) {
            ambiguous_unlink = false;
            errno = EINTR;
            return -1;
        }
        return r;
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
        const bool ok = call(RootOperation::Close, role, fd);
        const auto r = RootIo::close(fd); // Always consume once, even Before/Call rejection.
        if (close_fault && role == close_role && close_stage == RootStage::Call) {
            ++close_hits;
            if (reuse_close) {
                replacement = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                if (replacement < 0)
                    std::abort();
                if (replacement != fd) {
                    if (::dup2(replacement, fd) != fd || ::close(replacement) != 0)
                        std::abort();
                    replacement = fd;
                }
            }
            errno = close_errno;
            return -1;
        }
        if (!ok) {
            errno = fault_errno;
            return -1;
        }
        return invalid_close ? 1 : r;
    }
};
std::size_t scenarios = 0, injections = 0;
LogicalSequence seq(std::uint64_t n = 0) { return LogicalSequence{n}; }
PendingGeneration begin(RootLease& root, std::uint64_t g = 1, std::uint32_t budget = 8,
                        std::uint64_t sequence = 0) {
    return PendingGeneration::begin_after(root, gen(g), seq(sequence), budget);
}
void exact(RootError e, RootErrorCode code, RootOperation op = RootOperation::None,
           RootRole role = RootRole::Root, RootStage stage = RootStage::Call, int err = 0) {
    check(e == RootError{code, op, role, stage, err}, "exact first RootError");
}
void protected_entries(const Fixture::Inventory& before, const Fixture::Inventory& after) {
    for (const auto& [name, bytes] : before) {
        const auto it = after.find(name);
        check(it != after.end() && it->second == bytes,
              "protected bytes/type/inode/link/membership");
    }
}
void terminal(PendingGeneration& p, Adapter& io) {
    const auto first = p.error();
    check(first.code != RootErrorCode::None && !p.valid(), "terminal owner");
    const auto used = io.used;
    check(!p.generation() && !p.selected_generation() && !p.captured_sequence() &&
              !p.identity(ArtifactRole::Base) && !p.recheck() && !p.close_checked() &&
              !p.discard_checked() && p.error() == first && io.used == used,
          "terminal owner acted/recovered");
}
template <class Fn> void scenario(const std::string& name, Fn fn) {
    Fixture f;
    f.files();
    f.put("legacy.aof", std::string("V1\0AOF", 6));
    f.put("legacy.aof.commit", "V1 marker");
    f.put("generation-1.snap", "V1 snapshot");
    f.put("unowned", "leave intact");
    f.put(".hidden", "hidden sentinel");
    const auto before = f.inventory();
    auto io = std::make_shared<Adapter>();
    {
        auto root = RootLease::open_existing(f.path, io);
        check(root.valid() && io->live() == 2, "baseline root");
        io->forbid_reopen = true;
        try {
            fn(f, io, root);
        } catch (...) {
            std::cerr << "FAILED SCENARIO " << name << std::endl;
            throw;
        }
    }
    check(io->live() == 0 && io->opens == io->closes && io->tracking_ok && io->flags_ok,
          "FD leak/double-close/flags");
    protected_entries(before, f.inventory());
    ++scenarios;
    std::cout << "SCENARIO " << scenarios << " PASS " << name
              << " protected=LOCK,CURRENT,V1,V2,unowned; live=0\n";
}
int wait_bounded(pid_t pid, int options = 0);
void contention(const Fixture& f, const Adapter& io, bool held) {
    const auto pid = ::fork();
    check(pid >= 0, "contention fork");
    if (pid == 0) {
        for (const auto& d : io.descriptors)
            if (d.fd >= 0)
                (void)::close(d.fd);
        const auto fd = ::open((f.path / "LOCK").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            ::_exit(2);
        errno = 0;
        const auto r = ::flock(fd, LOCK_EX | LOCK_NB);
        const auto e = errno;
        (void)::close(fd);
        ::_exit(held ? (r == -1 && (e == EAGAIN || e == EWOULDBLOCK) ? 0 : 3) : (r == 0 ? 0 : 4));
    }
    const auto status = wait_bounded(pid);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "independent LOCK lifetime");
}
void positive_and_admission() {
    for (const auto selected : {std::uint64_t{1}, std::uint64_t{9}, UINT64_MAX - 1}) {
        scenario("three empty roles after " + std::to_string(selected), [=](auto& f, auto& io,
                                                                            auto& root) {
            const auto before = f.inventory();
            auto p = begin(root, selected, 1, 7);
            check(p.valid() && p.generation()->value() == selected + 1 &&
                      p.selected_generation()->value() == selected &&
                      p.captured_sequence()->value() == 7 && io->live() == 5 && io->creates == 3,
                  "complete typed acquisition");
            for (std::size_t i = 0; i < Roles.size(); ++i) {
                const auto* id = p.identity(Roles[i]);
                check(id && id->byte_length == 0, "zero extent");
                const auto& t = io->temps[i];
                struct stat st{};
                check(t.role == StagedRoles[i] &&
                          ::lstat((f.path / t.name.data()).c_str(), &st) == 0 &&
                          S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size == 0 &&
                          (st.st_mode & 0777) == 0600 && st.st_dev == id->device &&
                          st.st_ino == id->inode && (::fcntl(t.fd, F_GETFD) & FD_CLOEXEC) != 0 &&
                          std::strlen(t.name.data()) <= 69,
                      "exact role/identity/mode/names");
            }
            check(!p.identity(static_cast<ArtifactRole>(99)) && p.recheck() && root.valid(),
                  "local bad-role getter");
            auto second = begin(root, UINT64_MAX, 1, UINT64_MAX);
            exact(second.error(), RootErrorCode::WrongState);
            check(root.valid() && p.valid(), "busy poisoned owner");
            auto observation = GenerationCandidate::find_after(root, gen(selected), seq(7), 1);
            check(observation.valid() && *observation.generation() == *p.generation(),
                  "raw duplicate changed");
            check(p.discard_checked() && io->live() == 2 && f.inventory() == before,
                  "healthy exact discard");
            terminal(p, *io);
            auto third = begin(root, selected, 1, 7);
            check(third.valid(), "slot not released");
            check(third.close_checked() && io->live() == 2, "retain drains children");
            terminal(third, *io);
            auto fourth = begin(root, selected, 1, 7);
            check(fourth.valid(), "old closed owner retained slot");
        });
    }
    scenario(
        "raw staging remains independent of pending admission", [](auto&, auto& io, auto& root) {
            auto p = begin(root);
            auto raw = StagedFile::create_data(root, ArtifactRole::Base, gen(2));
            check(raw.valid() && p.recheck(), "raw stage admission changed");
            io->forbid_content = false;
            std::array<std::byte, 2> bytes{std::byte{0}, std::byte{255}}, read{};
            check(raw.append(bytes) && raw.read_exact(0, read) && read == bytes && raw.seal_file(),
                  "raw staging API changed");
            io->forbid_content = true;
            check(raw.discard_checked() && p.discard_checked(), "independent raw cleanup");
            std::cout << "RAW_STAGING_ONLY writes=" << io->writes << " reads=" << io->reads
                      << " syncs=" << io->syncs << '\n';
        });
    scenario("healthy destructor and real entropy", [](auto& f, auto& io, auto& root) {
        const auto before = f.inventory();
        io->real_entropy = true;
        {
            auto p = begin(root);
            check(p.valid() && p.recheck(), "OS entropy acquisition");
        }
        check(f.inventory() == before && io->unlinks == 3 && io->live() == 2,
              "destructor exact cleanup");
    });
    scenario("default/moved roots and owners", [](auto&, auto& io, auto& root) {
        PendingGeneration p;
        terminal(p, *io);
        RootLease absent;
        auto bad = begin(absent);
        exact(bad.error(), RootErrorCode::ClosedOrFailed);
        terminal(bad, *io);
        auto moved = std::move(root);
        auto from = begin(root);
        exact(from.error(), RootErrorCode::ClosedOrFailed);
        check(moved.close_checked(), "close root");
        auto closed = begin(moved);
        exact(closed.error(), RootErrorCode::ClosedOrFailed);
    });
    for (auto budget : {0U, 4097U, UINT32_MAX}) {
        scenario("local budget " + std::to_string(budget), [=](auto&, auto& io, auto& root) {
            auto p = begin(root);
            const auto n = io->used;
            auto bad = begin(root, 1, budget);
            exact(bad.error(), RootErrorCode::ResourceLimit);
            check(io->used == n && root.valid() && p.valid(),
                  "budget local precedence before busy");
            terminal(bad, *io);
            check(p.close_checked(), "release active owner");
            auto good = begin(root, 1, 1);
            check(good.valid(), "local reject stole slot");
        });
    }
    for (auto budget : {1U, 2U, 4096U})
        for (bool free_last : {false, true}) {
            scenario("budget boundary " + std::to_string(budget) + "/" + std::to_string(free_last),
                     [=](auto&, auto& io, auto& root) {
                         io->capture = false;
                         io->occupied_through = free_last ? budget : budget + 1;
                         auto p = begin(root, 1, budget);
                         if (free_last)
                             check(p.valid() && p.generation()->value() == budget + 1,
                                   "last allowed candidate");
                         else {
                             exact(p.error(), RootErrorCode::ResourceLimit);
                             terminal(p, *io);
                         }
                         io->occupied_through = 0;
                         io->reset();
                     });
        }
    for (bool sequence : {false, true}) {
        scenario("entered exhaustion " + std::to_string(sequence),
                 [=](auto&, auto& io, auto& root) {
                     auto p = begin(root, UINT64_MAX, 1, sequence ? UINT64_MAX : 0);
                     exact(p.error(), sequence ? RootErrorCode::SequenceExhausted
                                               : RootErrorCode::GenerationExhausted);
                     check(!root.valid() && io->creates == 0 && io->probes == 0,
                           "exhaustion created artifacts");
                     const auto first = p.error();
                     auto bad = begin(root, 1, 0);
                     check(bad.error() == first, "existing error precedence");
                     terminal(p, *io);
                 });
    }
    scenario("occupied maximum is true generation exhaustion", [](auto&, auto& io, auto& root) {
        io->all_occupied = true;
        auto p = begin(root, UINT64_MAX - 1, 1);
        exact(p.error(), RootErrorCode::GenerationExhausted);
        check(io->creates == 0, "max created");
        terminal(p, *io);
    });
    scenario("strong generation zero and spelling fail outside entry",
             [](auto&, auto& io, auto& root) {
                 const auto used = io->used;
                 for (const auto* text : {"0", "01", "-1", "18446744073709551616"}) {
                     bool rejected = false;
                     try {
                         (void)parse_generation(text);
                     } catch (const FormatError&) {
                         rejected = true;
                     }
                     check(rejected, "invalid generation accepted");
                 }
                 bool rejected = false;
                 try {
                     (void)gen(0);
                 } catch (const FormatError&) {
                     rejected = true;
                 }
                 check(rejected && root.valid() && io->used == used, "type rejection touched root");
             });
}
void make_entry(const Fixture& f, const char* name, int kind) {
    const auto p = f.path / name;
    if (kind == 0)
        f.put(name, "occupied");
    if (kind == 1)
        check(::mkdir(p.c_str(), 0700) == 0, "fixture directory");
    if (kind == 2)
        check(::symlink("unowned", p.c_str()) == 0, "fixture symlink");
    if (kind == 3)
        check(::symlink("absent-target", p.c_str()) == 0, "fixture dangling link");
    if (kind == 4)
        check(::mkfifo(p.c_str(), 0600) == 0, "fixture fifo");
    if (kind == 5) {
        const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        check(fd >= 0, "fixture socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto text = p.string();
        check(text.size() < sizeof(address.sun_path), "socket bound name");
        std::strcpy(address.sun_path, text.c_str());
        const auto r = ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        check(::close(fd) == 0 && r == 0, "owned socket bind/close");
    }
    if (kind == 6) {
        f.put("link-source", "independent hardlink sentinel");
        check(::link((f.path / "link-source").c_str(), p.c_str()) == 0, "fixture hardlink");
    }
}
std::string zero_temp(ArtifactRole role) {
    const auto suffix = role == ArtifactRole::Base   ? ".snap.tmp."
                        : role == ArtifactRole::Tail ? ".aof.tmp."
                                                     : ".commit.tmp.";
    return std::string(role == ArtifactRole::Base ? "base-2" : "tail-2") + suffix +
           std::string(32, '0');
}
void occupancy_and_scripts() {
    for (std::size_t role = 0; role < Roles.size(); ++role)
        for (int kind = 0; kind < 7; ++kind) {
            scenario("final entry type " + std::to_string(role) + "/" + std::to_string(kind),
                     [=](auto& f, auto& io, auto& root) {
                         make_entry(f, FinalNames[role], kind);
                         const auto before = f.inventory();
                         {
                             auto p = begin(root);
                             check(p.valid() && p.generation()->value() == 3,
                                   "occupied triple not skipped");
                         }
                         check(f.inventory() == before && io->reads == 0 && io->writes == 0 &&
                                   io->syncs == 0,
                               "occupied entry altered/followed");
                     });
        }
    for (std::size_t role = 0; role < Roles.size(); ++role) {
        scenario("late final collision retains all role " + std::to_string(role), [=](auto& f,
                                                                                      auto& io,
                                                                                      auto& root) {
            auto p = begin(root);
            f.put(FinalNames[role], "late entry");
            const auto before = f.inventory();
            check(!p.recheck() && io->live() == 2 && io->unlinks == 0 && f.inventory() == before,
                  "late collision cleanup/descriptor failure");
            exact(p.error(), RootErrorCode::CandidateOccupied, RootOperation::NamedStat,
                  static_cast<RootRole>(static_cast<int>(RootRole::Base) + static_cast<int>(role)));
            terminal(p, *io);
        });
    }
    std::array<std::size_t, 128> ordinals{};
    std::array<RootRole, 128> probe_roles{};
    std::array<std::array<char, 70>, 128> probe_names{};
    std::size_t count = 0;
    scenario("discover every acquisition candidate-probe boundary",
             [&](auto&, auto& io, auto& root) {
                 io->reset();
                 auto p = begin(root);
                 check(p.valid(), "occupancy discovery");
                 for (std::size_t i = 0; i < io->used; ++i) {
                     const auto& e = io->trace[i].event;
                     if (e.operation == RootOperation::NamedStat && e.stage == RootStage::Call &&
                         final_role(e.role)) {
                         check(count < ordinals.size(), "probe catalogue capacity");
                         ordinals[count] = count + 1;
                         probe_roles[count] = e.role;
                         probe_names[count] = io->trace[i].name;
                         ++count;
                     }
                 }
                 check(count > 9, "missing confirmation/acquisition/final recheck probes");
             });
    for (std::size_t i = 0; i < count; ++i) {
        scenario("entry appears at discovered candidate probe " + std::to_string(ordinals[i]),
                 [=](auto& f, auto& io, auto& root) {
                     io->occupy_probe = ordinals[i];
                     auto p = begin(root);
                     if (i < 3) {
                         check(p.valid() && p.generation()->value() == 3,
                               "initial probe collision must skip");
                     } else {
                         exact(p.error(), RootErrorCode::CandidateOccupied,
                               RootOperation::NamedStat, probe_roles[i]);
                         check(io->unlinks == 0 && io->live() == 2,
                               "post-observation collision must retain");
                         terminal(p, *io);
                     }
                     check(std::filesystem::exists(f.path / probe_names[i].data()),
                           "injected unowned collision removed");
                 });
    }
    for (std::size_t completed = 1; completed <= 3; ++completed) {
        scenario("collision immediately after create " + std::to_string(completed),
                 [=](auto&, auto& io, auto& root) {
                     io->occupy_after_create = completed;
                     auto p = begin(root);
                     check(!p.valid() && io->temp_count == completed && io->live() == 2 &&
                               io->unlinks == 0,
                           "partial collision disposition");
                     terminal(p, *io);
                 });
    }
    for (int eventual : {0, ENOENT, EIO}) {
        scenario("actual repeated candidate EINTR then " + std::to_string(eventual),
                 [=](auto&, auto& io, auto& root) {
                     io->interrupts = 3;
                     io->following_errno = eventual;
                     auto p = begin(root);
                     check(io->interrupts == 0, "actual retries not reached");
                     if (eventual == EIO) {
                         exact(p.error(), RootErrorCode::IoFailure, RootOperation::NamedStat,
                               RootRole::Base, RootStage::Call, EIO);
                         terminal(p, *io);
                     } else
                         check(p.valid(), "actual absence/retry rejected");
                 });
    }
    for (const auto& answer : {std::pair{1, 0}, std::pair{-2, EIO}, std::pair{-1, 0},
                               std::pair{-1, -7}, std::pair{-1, EACCES}, std::pair{-1, EIO}}) {
        scenario("invalid/error probe " + std::to_string(answer.first) + "/" +
                     std::to_string(answer.second),
                 [=](auto&, auto& io, auto& root) {
                     io->answer_at = 1;
                     io->answer_result = answer.first;
                     io->answer_errno = answer.second;
                     io->clobber_errno = ENOSPC;
                     auto p = begin(root);
                     const auto expected =
                         answer.first == -1 && answer.second > 0 ? answer.second : EIO;
                     exact(p.error(), RootErrorCode::IoFailure, RootOperation::NamedStat,
                           RootRole::Base, RootStage::Call, expected);
                     check(io->creates == 0, "bad probe created temps");
                     terminal(p, *io);
                 });
    }
    scenario("later role error is not hidden by earlier occupancy",
             [](auto& f, auto& io, auto& root) {
                 f.put("base-2.snap", "occupied");
                 io->answer_at = 3;
                 io->answer_errno = EIO;
                 auto p = begin(root);
                 exact(p.error(), RootErrorCode::IoFailure, RootOperation::NamedStat,
                       RootRole::Watermark, RootStage::Call, EIO);
                 check(io->probes == 3 && io->creates == 0, "all three roles/error masked");
                 terminal(p, *io);
             });
}
void creates_and_metadata() {
    for (std::size_t role = 0; role < Roles.size(); ++role) {
        for (unsigned failures : {63U, 64U}) {
            scenario("per-role collision boundary " + std::to_string(role) + "/" +
                         std::to_string(failures),
                     [=](auto&, auto& io, auto& root) {
                         io->collision_role = StagedRoles[role];
                         io->collisions = failures;
                         auto p = begin(root);
                         check(io->creates == (failures == 63 ? std::size_t{66} : role + 64) &&
                                   io->collisions == 0,
                               "64 actual create-attempt cap");
                         if (failures == 63)
                             check(p.valid() && io->temp_count == 3,
                                   "last collision slot did not succeed");
                         else {
                             exact(p.error(), RootErrorCode::CollisionLimit,
                                   RootOperation::CreateAt, StagedRoles[role], RootStage::Call,
                                   EEXIST);
                             check(io->temp_count == role && io->unlinks == 0 && io->live() == 2,
                                   "collision cap partial retention");
                             terminal(p, *io);
                         }
                     });
        }
        for (int err : {ENOSPC, EACCES, EIO, 0}) {
            scenario("create failure after earlier roles " + std::to_string(role) + "/" +
                         std::to_string(err),
                     [=](auto&, auto& io, auto& root) {
                         io->create_error_role = StagedRoles[role];
                         io->create_errno = err;
                         auto p = begin(root);
                         exact(p.error(), RootErrorCode::IoFailure, RootOperation::CreateAt,
                               StagedRoles[role], RootStage::Call, err ? err : EIO);
                         check(io->temp_count == role && io->live() == 2 && io->unlinks == 0,
                               "partial create failure didn't retain/drain");
                         terminal(p, *io);
                     });
        }
        for (int result : {-1, 1, -2}) {
            scenario("entropy malformed/error role " + std::to_string(role) + "/" +
                         std::to_string(result),
                     [=](auto&, auto& io, auto& root) {
                         io->entropy_role = StagedRoles[role];
                         io->entropy_result = result;
                         io->entropy_errno = EACCES;
                         auto p = begin(root);
                         exact(p.error(), RootErrorCode::IoFailure, RootOperation::Random128,
                               StagedRoles[role], RootStage::Call, result == -1 ? EACCES : EIO);
                         check(io->temp_count == role && io->unlinks == 0 && io->live() == 2,
                               "entropy failed after temp ownership");
                         terminal(p, *io);
                     });
        }
        for (auto edit : {Adapter::Edit::Inode, Adapter::Edit::Device, Adapter::Edit::Type,
                          Adapter::Edit::Links, Adapter::Edit::Size, Adapter::Edit::Negative,
                          Adapter::Edit::Huge}) {
            scenario("fresh metadata role " + std::to_string(role) + "/" +
                         std::to_string(static_cast<int>(edit)),
                     [=](auto&, auto& io, auto& root) {
                         io->edit_role = StagedRoles[role];
                         io->edit = edit;
                         auto p = begin(root);
                         check(!p.valid() && !root.valid() && io->temp_count == role + 1 &&
                                   io->unlinks == 0 && io->live() == 2,
                               "fresh metadata bypass/partial retention");
                         terminal(p, *io);
                     });
        }
    }
    for (int kind = 0; kind < 7; ++kind) {
        scenario("actual unowned temp collision type " + std::to_string(kind),
                 [=](auto& f, auto& io, auto& root) {
                     const auto name = zero_temp(ArtifactRole::Base);
                     make_entry(f, name.c_str(), kind);
                     const auto before = f.inventory();
                     io->fixed_token = true;
                     auto p = begin(root);
                     exact(p.error(), RootErrorCode::CollisionLimit, RootOperation::CreateAt,
                           RootRole::StagedBase, RootStage::Call, EEXIST);
                     check(io->creates == 64 && io->live() == 2 && f.inventory() == before,
                           "unowned collision altered");
                     terminal(p, *io);
                 });
    }
    scenario("ambiguous actual create EINTR leaves unknown name", [](auto& f, auto& io,
                                                                     auto& root) {
        io->ambiguous_create = true;
        auto p = begin(root);
        check(p.valid() && io->creates == 4 && io->temp_count == 4, "fresh token retry");
        check(!io->temps[0].known &&
                  std::strcmp(io->temps[0].name.data(), io->temps[1].name.data()) != 0,
              "ambiguous name incorrectly adopted/reused");
        const auto orphan = std::string(io->temps[0].name.data());
        check(p.discard_checked() && std::filesystem::exists(f.path / orphan) && io->unlinks == 3,
              "ambiguous name unlinked");
    });
    scenario("invalid fixed-result stat", [](auto&, auto& io, auto& root) {
        io->invalid_stat_result = 1;
        auto p = begin(root);
        exact(p.error(), RootErrorCode::IoFailure, RootOperation::Stat, RootRole::Root,
              RootStage::Call, EIO);
        terminal(p, *io);
    });
    scenario("invalid negative create result", [](auto&, auto& io, auto& root) {
        io->create_error_role = RootRole::StagedBase;
        io->create_result = -2;
        auto p = begin(root);
        exact(p.error(), RootErrorCode::IoFailure, RootOperation::CreateAt, RootRole::StagedBase,
              RootStage::Call, EIO);
        terminal(p, *io);
    });
    for (bool named : {false, true})
        for (std::size_t role = 0; role < Roles.size(); ++role)
            for (auto edit : {Adapter::Edit::Inode, Adapter::Edit::Device, Adapter::Edit::Type,
                              Adapter::Edit::Links, Adapter::Edit::Size, Adapter::Edit::Negative,
                              Adapter::Edit::Huge}) {
                scenario("owned metadata change " + std::to_string(named) + "/" +
                             std::to_string(role) + "/" + std::to_string(static_cast<int>(edit)),
                         [=](auto& f, auto& io, auto& root) {
                             auto p = begin(root);
                             const auto before = f.inventory();
                             io->edit_role = StagedRoles[role];
                             io->edit = edit;
                             io->edit_named = named;
                             check(!p.recheck() && io->live() == 2 && io->unlinks == 0 &&
                                       f.inventory() == before,
                                   "owned identity failure changed files");
                             terminal(p, *io);
                         });
            }
    for (std::size_t role = 0; role < Roles.size(); ++role)
        for (int kind = 0; kind < 4; ++kind) {
            scenario("real owned temp mutation " + std::to_string(role) + "/" +
                         std::to_string(kind),
                     [=](auto& f, auto& io, auto& root) {
                         auto p = begin(root);
                         const auto name = std::string(io->temps[role].name.data());
                         const auto path = f.path / name;
                         if (kind == 0) {
                             f.remove(name.c_str());
                             f.put(name.c_str(), "replacement");
                         }
                         if (kind == 1)
                             check(::truncate(path.c_str(), 1) == 0, "fixture extends temp");
                         if (kind == 2)
                             check(::link(path.c_str(), (f.path / "extra-alias").c_str()) == 0,
                                   "fixture temp hardlink");
                         if (kind == 3) {
                             f.remove(name.c_str());
                             check(::symlink("unowned", path.c_str()) == 0, "fixture temp symlink");
                         }
                         const auto before = f.inventory();
                         check(!p.recheck() && io->unlinks == 0 && f.inventory() == before,
                               "real temp mutation was cleaned/ignored");
                         terminal(p, *io);
                     });
        }
    for (auto role : {RootRole::Root, RootRole::LockFile})
        for (bool named : {false, true}) {
            if (role == RootRole::Root && named)
                continue;
            scenario("held root/LOCK identity " + std::to_string(static_cast<int>(role)) + "/" +
                         std::to_string(named),
                     [=](auto& f, auto& io, auto& root) {
                         auto p = begin(root);
                         const auto before = f.inventory();
                         io->edit_role = role;
                         io->edit_named = named;
                         check(!p.recheck() && io->unlinks == 0 && f.inventory() == before,
                               "root/LOCK mutation bypass");
                         terminal(p, *io);
                     });
        }
    for (int kind = 0; kind < 4; ++kind) {
        scenario(
            "real named LOCK change " + std::to_string(kind), [=](auto& f, auto& io, auto& root) {
                auto p = begin(root);
                check(::rename((f.path / "LOCK").c_str(), (f.path / "old-lock").c_str()) == 0,
                      "save LOCK");
                if (kind == 0)
                    f.put("LOCK", "");
                if (kind == 1)
                    check(::symlink("old-lock", (f.path / "LOCK").c_str()) == 0, "LOCK symlink");
                if (kind == 2)
                    check(::link((f.path / "old-lock").c_str(), (f.path / "LOCK").c_str()) == 0,
                          "LOCK hardlink");
                check(!p.recheck() && io->unlinks == 0, "changed LOCK accepted");
                if (kind != 3)
                    f.remove("LOCK");
                check(::rename((f.path / "old-lock").c_str(), (f.path / "LOCK").c_str()) == 0,
                      "restore fixture LOCK");
                terminal(p, *io);
            });
    }
    scenario(
        "held directory survives original pathname replacement", [](auto& f, auto&, auto& root) {
            const auto held = std::filesystem::path(f.path.string() + "-held");
            check(::rename(f.path.c_str(), held.c_str()) == 0 && ::mkdir(f.path.c_str(), 0700) == 0,
                  "replace root pathname");
            auto p = begin(root);
            const bool healthy = p.valid() && p.recheck() && p.discard_checked();
            check(::rmdir(f.path.c_str()) == 0 && ::rename(held.c_str(), f.path.c_str()) == 0,
                  "restore held directory");
            check(healthy, "root pathname reopened");
        });
}
void lifetimes_moves_and_closes() {
    scenario("move construction/self move/successful assignment guard ownership",
             [](auto&, auto& io, auto& root) {
                 auto first = begin(root);
                 const auto id = *first.identity(ArtifactRole::Base);
                 const auto used = io->used;
                 auto moved = std::move(first);
                 check(io->used == used && moved.valid() && !first.valid() &&
                           *moved.identity(ArtifactRole::Base) == id,
                       "move transfer did I/O");
                 auto* self = &moved;
                 moved = std::move(*self);
                 check(io->used == used && moved.valid(), "self move");
                 first = PendingGeneration{};
                 auto rejected = begin(root);
                 exact(rejected.error(), RootErrorCode::WrongState);
                 PendingGeneration target;
                 target = std::move(moved);
                 check(target.valid() && !moved.valid() &&
                           *target.identity(ArtifactRole::Base) == id,
                       "successful target move");
                 moved = PendingGeneration{};
                 auto again = begin(root);
                 exact(again.error(), RootErrorCode::WrongState);
                 check(target.discard_checked(), "move target discard");
                 auto last = begin(root);
                 check(last.valid(), "moved-from cleared/stuck slot");
             });
    scenario("whole owner close releases child lease while pin retains root",
             [](auto& f, auto& io, auto& root) {
                 io->forbid_reopen = false;
                 auto pin = root.pin_current();
                 io->forbid_reopen = true;
                 auto raw = StagedFile::create_data(root, ArtifactRole::Tail, gen(3));
                 auto observed = GenerationCandidate::find_after(root, gen(1), seq(), 1);
                 auto p = begin(root);
                 root = RootLease{};
                 check(p.recheck() && pin.recheck() && raw.recheck(), "lost shared root lifetime");
                 contention(f, *io, true);
                 check(p.close_checked(), "whole owner retain");
                 contention(f, *io, true);
                 raw = StagedFile{};
                 observed = GenerationCandidate{};
                 contention(f, *io, true);
                 pin = PinnedFile{};
                 check(io->live() == 0, "last peer did not close root");
                 contention(f, *io, false);
             });
    scenario("whole owner terminal operation drops all child references",
             [](auto&, auto& io, auto& root) {
                 auto p = begin(root);
                 check(p.close_checked() && io->live() == 2, "retain did not drain children");
                 check(root.close_checked() && io->live() == 0, "closed aggregate still dependent");
                 terminal(p, *io);
             });
    scenario("premature RootLease close is terminal DependentsAlive",
             [](auto& f, auto& io, auto& root) {
                 auto p = begin(root);
                 check(!root.close_checked(), "premature root close succeeded");
                 const auto first = root.error();
                 exact(first, RootErrorCode::DependentsAlive, RootOperation::Close);
                 contention(f, *io, true);
                 check(!p.recheck() && p.error() == first && io->live() == 2 && io->unlinks == 0,
                       "dependent failure not retained");
                 terminal(p, *io);
                 contention(f, *io, true);
                 root = RootLease{};
                 contention(f, *io, false);
             });
    for (auto role : {RootRole::StagedBase, RootRole::StagedTail, RootRole::StagedWatermark,
                      RootRole::LockFile, RootRole::Root})
        for (auto stage : {RootStage::Before, RootStage::Call, RootStage::After})
            for (int err : {EIO, EINTR}) {
                scenario("checked final release error " + std::to_string(static_cast<int>(role)) +
                             "/" + std::to_string(static_cast<int>(stage)) + "/" +
                             std::to_string(err),
                         [=](auto& f, auto& io, auto& root) {
                             auto p = begin(root);
                             root = RootLease{};
                             const auto closes = io->closes;
                             io->close_fault = true;
                             io->close_role = role;
                             io->close_stage = stage;
                             io->close_errno = err;
                             io->reuse_close = stage == RootStage::Call;
                             check(!p.close_checked() && io->closes == closes + 5 &&
                                       io->live() == 0 && io->close_hits == 1,
                                   "final release lost/duplicated close");
                             exact(p.error(), RootErrorCode::IoFailure, RootOperation::Close, role,
                                   stage, err);
                             terminal(p, *io);
                             const auto used = io->used;
                             p = PendingGeneration{};
                             check(io->used == used, "failed assignment retried resource");
                             if (io->replacement >= 0) {
                                 check(::fcntl(io->replacement, F_GETFD) >= 0 &&
                                           ::close(io->replacement) == 0,
                                       "consumed FD number reused/double closed");
                                 io->replacement = -1;
                             }
                             contention(f, *io, false);
                         });
            }
    for (auto role : {RootRole::StagedBase, RootRole::LockFile, RootRole::Root}) {
        scenario("destructor last-reference close failure " +
                     std::to_string(static_cast<int>(role)),
                 [=](auto&, auto& io, auto& root) {
                     {
                         auto p = begin(root);
                         root = RootLease{};
                         io->close_fault = true;
                         io->close_role = role;
                         io->close_stage = RootStage::Before;
                     }
                     check(io->live() == 0 && io->closes == 5 && io->close_hits == 1,
                           "destructor must consume all once");
                 });
    }
    for (auto role : {RootRole::StagedBase, RootRole::LockFile, RootRole::Root}) {
        scenario("failed target disposal leaves incoming intact " +
                     std::to_string(static_cast<int>(role)),
                 [=](auto&, auto& io, auto& root) {
                     Fixture other;
                     other.files();
                     const auto before = other.inventory();
                     auto other_io = std::make_shared<Adapter>();
                     {
                         auto other_root = RootLease::open_existing(other.path, other_io);
                         other_io->forbid_reopen = true;
                         auto source = begin(other_root);
                         const auto identity = *source.identity(ArtifactRole::Base);
                         auto target = begin(root);
                         root = RootLease{};
                         io->close_fault = true;
                         io->close_role = role;
                         io->close_stage = RootStage::Call;
                         io->close_errno = EIO;
                         const auto source_used = other_io->used;
                         target = std::move(source);
                         exact(target.error(), RootErrorCode::IoFailure, RootOperation::Close, role,
                               RootStage::Call, EIO);
                         check(!target.valid() && source.valid() && other_root.valid() &&
                                   *source.identity(ArtifactRole::Base) == identity &&
                                   other_io->used == source_used && io->live() == 0,
                               "failed disposal consumed/poisoned incoming root");
                         target = std::move(source);
                         check(source.valid() && other_io->used == source_used,
                               "already-failed target consumed source");
                         check(source.recheck() && source.discard_checked(),
                               "incoming owner damaged");
                         auto next = begin(other_root);
                         check(next.valid() && next.discard_checked(),
                               "incoming root admission damaged");
                     }
                     check(other_io->live() == 0 && other_io->opens == other_io->closes &&
                               other.inventory() == before,
                           "independent root preservation");
                 });
    }
    scenario("healthy cross-root target assignment discards old then transfers", [](auto&, auto& io,
                                                                                    auto& root) {
        Fixture other;
        other.files();
        auto other_io = std::make_shared<Adapter>();
        auto other_root = RootLease::open_existing(other.path, other_io);
        other_io->forbid_reopen = true;
        auto source = begin(other_root);
        auto target = begin(root);
        target = std::move(source);
        check(target.valid() && !source.valid() && io->live() == 2 && io->unlinks == 3 &&
                  root.valid(),
              "healthy target disposal/transfer");
        auto old_root_reuse = begin(root);
        check(old_root_reuse.valid() && target.discard_checked(), "source slot or prior slot lost");
    });
    for (int err : {EIO, ENOENT, EINTR}) {
        scenario("unlink error retains remaining " + std::to_string(err),
                 [=](auto& f, auto& io, auto& root) {
                     auto p = begin(root);
                     const auto before = f.inventory();
                     io->unlink_errno = err;
                     check(!p.discard_checked() && io->unlinks == 1 && io->live() == 2 &&
                               f.inventory() == before,
                           "unlink failure retried/cleaned later roles");
                     exact(p.error(), RootErrorCode::IoFailure, RootOperation::UnlinkAt,
                           RootRole::StagedBase, RootStage::Call, err);
                     terminal(p, *io);
                 });
    }
    scenario("ambiguous successful unlink reported EINTR is never retried",
             [](auto& f, auto& io, auto& root) {
                 auto p = begin(root);
                 const auto first = std::string(io->temps[0].name.data());
                 io->ambiguous_unlink = true;
                 check(!p.discard_checked() && io->unlinks == 1 &&
                           !std::filesystem::exists(f.path / first) && io->live() == 2,
                       "ambiguous unlink disposition");
                 terminal(p, *io);
             });
    scenario("invalid close result consumes descriptors and latches EIO",
             [](auto&, auto& io, auto& root) {
                 auto p = begin(root);
                 io->invalid_close = true;
                 check(!p.close_checked() && io->live() == 2, "invalid close accepted/leaked");
                 exact(p.error(), RootErrorCode::IoFailure, RootOperation::Close,
                       RootRole::StagedBase, RootStage::Call, EIO);
                 terminal(p, *io);
             });
    scenario("earlier root error wins over all subsequent close faults", [](auto&, auto& io,
                                                                            auto& root) {
        auto p = begin(root);
        io->edit_named = true;
        io->edit_role = RootRole::LockFile;
        io->close_fault = true;
        io->close_role = RootRole::StagedBase;
        io->close_stage = RootStage::Call;
        check(!p.recheck(), "failed root check accepted");
        exact(p.error(), RootErrorCode::IdentityChanged, RootOperation::NamedStat,
              RootRole::LockFile);
        check(io->close_hits == 1 && io->unlinks == 0 && io->live() == 2, "first-error/retention");
        terminal(p, *io);
    });
    scenario("zero reached allocations across post-root lifecycle",
             [](auto&, auto& io, auto& root) {
                 const auto allocations = guarded_allocations, rejects = allocation_rejections;
                 allocation_budget = 0;
                 auto p = begin(root);
                 const bool a = p.valid() && p.recheck();
                 auto q = std::move(p);
                 auto* self = &q;
                 q = std::move(*self);
                 PendingGeneration moved;
                 moved = std::move(q);
                 const bool b = moved.close_checked();
                 auto again = begin(root);
                 const bool c = again.valid() && again.discard_checked();
                 allocation_budget = -1;
                 check(a && b && c && io->live() == 2 && guarded_allocations == allocations &&
                           allocation_rejections == rejects,
                       "unexpected allocation or lost claim");
                 std::cout << "ALLOCATION pending lifecycle: zero reached allocation sites\n";
             });
    scenario("genuinely reached existing RootState allocation failure propagates",
             [](auto& f, auto& io, auto& root) {
                 root = RootLease{};
                 const auto rejected = allocation_rejections;
                 allocation_budget = 0;
                 auto failed_root = RootLease::open_existing(f.path, io);
                 auto p = begin(failed_root);
                 allocation_budget = -1;
                 check(allocation_rejections == rejected + 1 &&
                           p.error().code == RootErrorCode::ResourceLimit && io->live() == 0 &&
                           io->creates == 0,
                       "real allocation failure propagation");
                 terminal(p, *io);
                 std::cout << "ALLOCATION existing RootState failure actually reached=1\n";
             });
}
enum class Action { Acquire, Skip, Recheck, Discard, Retain, Destroy, LastClose };
constexpr std::array<const char*, 7> ActionNames{"acquire", "skip",       "recheck",   "discard",
                                                 "retain",  "destructor", "last-close"};
bool perform(Action action, PendingGeneration& p, RootLease& root) {
    switch (action) {
    case Action::Acquire:
    case Action::Skip:
        p = begin(root);
        return p.valid();
    case Action::Recheck:
        return p.recheck();
    case Action::Discard:
        return p.discard_checked();
    case Action::Retain:
    case Action::LastClose:
        return p.close_checked();
    case Action::Destroy: {
        std::optional<PendingGeneration> ending(std::move(p));
        ending.reset();
        return root.valid();
    }
    }
    return false;
}
void fault_matrix() {
    for (const auto action : {Action::Acquire, Action::Skip, Action::Recheck, Action::Discard,
                              Action::Retain, Action::Destroy, Action::LastClose}) {
        std::array<RootIoEvent, 2048> plan{};
        std::size_t count = 0;
        const auto label = std::string(ActionNames[static_cast<std::size_t>(action)]);
        scenario("discover actual trace " + label, [&](auto& f, auto& io, auto& root) {
            if (action == Action::Skip)
                f.put("base-2.snap", "occupied");
            PendingGeneration p;
            if (action != Action::Acquire && action != Action::Skip)
                p = begin(root);
            if (action == Action::LastClose)
                root = RootLease{};
            io->reset();
            check(perform(action, p, root), "healthy trace operation failed");
            count = io->used;
            check(count > 0 && count <= plan.size(), "trace capacity");
            for (std::size_t i = 0; i < count; ++i) {
                plan[i] = io->trace[i].event;
                std::cout << "PLAN " << label << ' ' << i + 1
                          << " op=" << static_cast<int>(plan[i].operation)
                          << " role=" << static_cast<int>(plan[i].role)
                          << " stage=" << static_cast<int>(plan[i].stage) << '\n';
            }
        });
        for (std::size_t ordinal = 1; ordinal <= count; ++ordinal) {
            scenario("reached " + label + " hook " + std::to_string(ordinal), [&](auto& f, auto& io,
                                                                                  auto& root) {
                if (action == Action::Skip)
                    f.put("base-2.snap", "occupied");
                PendingGeneration p;
                if (action != Action::Acquire && action != Action::Skip)
                    p = begin(root);
                if (action == Action::LastClose)
                    root = RootLease{};
                io->reset();
                io->target = ordinal;
                check(!perform(action, p, root), "injected operation succeeded");
                check(io->reached == 1 && io->used >= ordinal, "planned injection not reached");
                const auto expected = plan[ordinal - 1], actual = io->trace[ordinal - 1].event;
                check(actual.operation == expected.operation && actual.role == expected.role &&
                          actual.stage == expected.stage,
                      "trace diverged before injection");
                const auto e = action == Action::Destroy ? root.error() : p.error();
                exact(e, RootErrorCode::IoFailure, expected.operation, expected.role,
                      expected.stage, EIO);
                check(io->unlinks_after_fault == 0, "unlink after terminal fault");
                check(io->live() == (action == Action::LastClose ? 0 : 2),
                      "failed operation retained descriptor");
                if (action != Action::Destroy)
                    terminal(p, *io);
                ++injections;
                std::cout << "INJECTION " << label << " planned=" << ordinal
                          << " reached=" << io->reached
                          << " op=" << static_cast<int>(actual.operation)
                          << " role=" << static_cast<int>(actual.role)
                          << " stage=" << static_cast<int>(actual.stage)
                          << " first-error-preserved\n";
            });
        }
    }
}
int wait_bounded(pid_t pid, int options) {
    int status = 0;
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        const auto result = ::waitpid(pid, &status, options | WNOHANG);
        if (result == pid)
            return status;
        check(result == 0 || (result == -1 && errno == EINTR), "owned child wait");
        (void)::poll(nullptr, 0, 10);
    }
    (void)::kill(pid, SIGKILL);
    (void)::waitpid(pid, &status, 0);
    throw std::runtime_error("owned child bounded wait expired");
}
void crash_survival() {
    for (unsigned boundary = 0; boundary <= 4; ++boundary) {
        scenario("owned child interruption boundary " + std::to_string(boundary), [=](auto& f,
                                                                                      auto& io,
                                                                                      auto& root) {
            root = RootLease{};
            int pipe_fds[2]{};
            check(::pipe(pipe_fds) == 0, "child pipe");
            const auto pid = ::fork();
            check(pid >= 0, "crash child fork");
            if (pid == 0) {
                (void)::close(pipe_fds[0]);
                auto child_io = std::make_shared<Adapter>();
                auto child_root = RootLease::open_existing(f.path, child_io);
                if (!child_root.valid())
                    ::_exit(93);
                child_io->forbid_reopen = true;
                child_io->pause_fd = boundary < 4 ? pipe_fds[1] : -1;
                child_io->pause_before = boundary == 0;
                child_io->pause_create = boundary;
                auto child_owner = begin(child_root);
                if (!child_owner.valid())
                    ::_exit(94);
                const char ready = '!';
                if (::write(pipe_fds[1], &ready, 1) != 1)
                    ::_exit(95);
                (void)::raise(SIGSTOP);
                ::_exit(96);
            }
            (void)::close(pipe_fds[1]);
            struct pollfd ready{pipe_fds[0], POLLIN, 0};
            const auto available = ::poll(&ready, 1, 5000);
            if (available != 1 || !(ready.revents & POLLIN)) {
                (void)::kill(pid, SIGKILL);
                (void)wait_bounded(pid);
                (void)::close(pipe_fds[0]);
                check(false, "owned child did not reach crash boundary");
            }
            char byte = 0;
            check(::read(pipe_fds[0], &byte, 1) == 1 && byte == '!', "child boundary notification");
            (void)::close(pipe_fds[0]);
            const auto stopped = wait_bounded(pid, WUNTRACED);
            check(WIFSTOPPED(stopped), "child not stopped");
            check(::kill(pid, SIGKILL) == 0, "kill owned child");
            const auto killed = wait_bounded(pid);
            check(WIFSIGNALED(killed) && WTERMSIG(killed) == SIGKILL, "child not killed");
            const auto retained = f.inventory();
            std::size_t temps = 0;
            for (const auto& [name, bytes] : retained) {
                (void)bytes;
                if (name.find(".tmp.") != std::string::npos)
                    ++temps;
            }
            check(temps == (boundary == 4 ? 3 : boundary), "crash empty-temp survival count");
            auto reopened = RootLease::open_existing(f.path);
            check(reopened.valid(), "crash LOCK not released");
            auto next = begin(reopened);
            check(next.valid() && next.discard_checked(), "crash temps wrongly claimed/reclaimed");
            check(f.inventory() == retained && io->live() == 0,
                  "crash temp/selected-file preservation");
            std::cout << "CRASH boundary=" << boundary << " surviving-empty-temps=" << temps
                      << " owned-SIGKILL-only; no power-loss certification\n";
        });
    }
}

// Independent cross-root oracle: snapshots are owned values, never borrowed
// identities retained across a move/terminal operation. Existing traces/tests
// above remain unchanged and run before this strictly additive campaign.
struct MoveContext {
    std::uint64_t generation, selected, captured;
    std::array<FileIdentity, 3> identities;
};
MoveContext move_context(const PendingGeneration& owner) {
    check(owner.valid(), "move snapshot requires healthy owner");
    MoveContext context{owner.generation()->value(),
                        owner.selected_generation()->value(),
                        owner.captured_sequence()->value(),
                        {}};
    for (std::size_t i = 0; i < Roles.size(); ++i) {
        check(owner.identity(Roles[i]) != nullptr, "move snapshot missing role");
        context.identities[i] = *owner.identity(Roles[i]);
        check(context.identities[i].byte_length == 0, "move snapshot nonempty role");
    }
    return context;
}
void same_move_context(const PendingGeneration& owner, const MoveContext& context) {
    check(owner.valid() && owner.generation()->value() == context.generation &&
              owner.selected_generation()->value() == context.selected &&
              owner.captured_sequence()->value() == context.captured,
          "incoming context consumed or changed");
    for (std::size_t i = 0; i < Roles.size(); ++i)
        check(owner.identity(Roles[i]) && *owner.identity(Roles[i]) == context.identities[i],
              "incoming borrowed identity changed");
}
void move_empty_files(const Fixture& fixture, const Adapter& io, const MoveContext& context) {
    check(io.live() == 5 && io.temp_count == 3 && io.creates == 3,
          "move fixture owns wrong FD count");
    for (std::size_t i = 0; i < Roles.size(); ++i) {
        const auto& t = io.temps[i];
        struct stat named{}, held{};
        check(t.known && !t.removed && t.role == StagedRoles[i] &&
                  ::lstat((fixture.path / t.name.data()).c_str(), &named) == 0 &&
                  ::fstat(t.fd, &held) == 0 && S_ISREG(named.st_mode) && named.st_nlink == 1 &&
                  named.st_size == 0 && held.st_size == 0 && named.st_dev == held.st_dev &&
                  named.st_ino == held.st_ino && named.st_dev == context.identities[i].device &&
                  named.st_ino == context.identities[i].inode &&
                  (::fcntl(t.fd, F_GETFD) & FD_CLOEXEC) != 0,
              "move fixture exact empty file identity");
    }
}
void cross_root_move_target_disposal() {
    constexpr const char* Campaign = "cross_root_move_target_disposal";
    std::size_t total_planned = 0, total_reached = 0, total_validated = 0;
    for (const bool final_reference : {false, true}) {
        const auto variant = std::string(final_reference ? "final_reference" : "lease_held");
        std::array<Adapter::Trace, 2048> plan{};
        std::size_t count = 0;
        std::array<std::size_t, 3> stages{};
        std::size_t executed = 0, reached = 0, validated = 0;
        // Each invocation runs inside a fresh A fixture supplied by scenario(),
        // and constructs an independent B fixture/adapter/lease/source here.
        const auto run = [&](Fixture& a, std::shared_ptr<Adapter>& a_io, RootLease& a_root,
                             std::size_t ordinal) {
            Fixture b;
            b.files();
            b.put("legacy.aof", std::string("B\0V1", 4));
            b.put("unowned", "source sentinel");
            b.put(".hidden", "source hidden");
            const auto b_baseline = b.inventory();
            const auto a_baseline = a.inventory();
            check(a.device != b.device || a.inode != b.inode, "cross-root fixture aliases root");
            auto b_io = std::make_shared<Adapter>();
            {
                auto b_root = RootLease::open_existing(b.path, b_io);
                check(b_root.valid(), "incoming root acquisition");
                b_io->forbid_reopen = true;
                auto target = begin(a_root, 1, 8, 11);
                auto source = begin(b_root, 9, 8, 17);
                const auto a_context = move_context(target), b_context = move_context(source);
                move_empty_files(a, *a_io, a_context);
                move_empty_files(b, *b_io, b_context);
                const auto a_owned = a.inventory(), b_owned = b.inventory();
                const auto b_descriptors = b_io->descriptors;
                const auto b_used = b_io->used, b_opens = b_io->opens, b_closes = b_io->closes;
                const auto b_unlinks = b_io->unlinks;
                if (ordinal == 0) {
                    contention(a, *a_io, true);
                    contention(b, *b_io, true);
                }
                if (final_reference)
                    a_root = RootLease{};
                a_io->reset();
                a_io->target = ordinal;
                const auto allocations = guarded_allocations, rejects = allocation_rejections;
                allocation_budget = 0;
                target = std::move(source);
                allocation_budget = -1;
                check(guarded_allocations == allocations && allocation_rejections == rejects,
                      "move target disposal/transfer allocated");
                check(b_io->used == b_used && b_io->opens == b_opens && b_io->closes == b_closes &&
                          b_io->unlinks == b_unlinks && b_io->live() == 5 && b_root.valid() &&
                          b.inventory() == b_owned,
                      "A disposal touched incoming root or files");
                for (std::size_t i = 0; i < b_descriptors.size(); ++i)
                    check(b_io->descriptors[i].fd == b_descriptors[i].fd &&
                              b_io->descriptors[i].role == b_descriptors[i].role,
                          "incoming descriptor ownership changed");
                auto expected_a = a_owned;
                std::size_t removed = 0;
                for (std::size_t i = 0; i < Roles.size(); ++i)
                    if (a_io->temps[i].removed) {
                        expected_a.erase(a_io->temps[i].name.data());
                        ++removed;
                    }
                check(a.inventory() == expected_a && a_io->live() == (final_reference ? 0 : 2) &&
                          a_io->closes == (final_reference ? 5 : 3) &&
                          a_io->unlinks_after_fault == 0,
                      "target cleanup side effects or descriptor count incorrect");
                if (ordinal == 0) {
                    count = a_io->used;
                    check(count > 0 && count <= plan.size(), "move trace capacity");
                    for (std::size_t i = 0; i < count; ++i) {
                        plan[i] = a_io->trace[i];
                        ++stages[static_cast<std::size_t>(plan[i].event.stage)];
                    }
                    for (std::size_t i = 0; i < count; ++i) {
                        const auto& event = plan[i].event;
                        const auto call = event.stage == RootStage::Before  ? i + 1
                                          : event.stage == RootStage::After ? i - 1
                                                                            : i;
                        check(call < count && plan[call].event.stage == RootStage::Call &&
                                  plan[call].event.operation == event.operation &&
                                  plan[call].event.role == event.role,
                              "move healthy trace call association");
                        std::cout << "MOVE_PLAN " << Campaign << " variant=" << variant
                                  << " ordinal=" << i + 1
                                  << " op=" << static_cast<int>(event.operation)
                                  << " role=" << static_cast<int>(event.role)
                                  << " stage=" << static_cast<int>(event.stage)
                                  << " fd=" << plan[call].fd << " name=" << plan[call].name.data()
                                  << " result_known=" << (event.stage == RootStage::After)
                                  << " result=" << event.result << " errno=" << event.system_errno
                                  << '\n';
                    }
                    check(removed == 3 && a.inventory() == a_baseline && !source.valid(),
                          "healthy move disposal/consumption");
                    same_move_context(target, b_context);
                    source = PendingGeneration{};
                    same_move_context(target, b_context);
                    auto busy = begin(b_root);
                    exact(busy.error(), RootErrorCode::WrongState);
                    check(target.recheck() && target.discard_checked() && b_root.valid(),
                          "healthy transferred source usability");
                    if (!final_reference) {
                        auto next_a = begin(a_root);
                        check(next_a.valid() && next_a.discard_checked(),
                              "old A slot not released");
                    }
                    contention(a, *a_io, !final_reference);
                } else {
                    ++executed;
                    check(a_io->reached == 1 && a_io->used >= ordinal,
                          "move fault not reached exactly once");
                    ++reached;
                    const auto expected = plan[ordinal - 1].event,
                               actual = a_io->trace[ordinal - 1].event;
                    check(actual.operation == expected.operation && actual.role == expected.role &&
                              actual.stage == expected.stage,
                          "move fault reached wrong boundary");
                    exact(target.error(), RootErrorCode::IoFailure, expected.operation,
                          expected.role, expected.stage, EIO);
                    const auto first = target.error();
                    terminal(target, *a_io);
                    if (!final_reference)
                        check(a_root.error() == first && !a_root.valid(),
                              "target first-error latch absent");
                    same_move_context(source, b_context);
                    move_empty_files(b, *b_io, b_context);
                    check(source.recheck() && b_root.valid() &&
                              b_root.error().code == RootErrorCode::None,
                          "source unusable after target fault");
                    const auto source_used = b_io->used;
                    auto busy = begin(b_root);
                    exact(busy.error(), RootErrorCode::WrongState);
                    check(b_io->used == source_used && source.valid() && b_root.valid(),
                          "incoming admission lost/poisoned");
                    target = std::move(source);
                    check(target.error() == first && b_io->used == source_used,
                          "failed target retried/consumed source");
                    same_move_context(source, b_context);
                    contention(b, *b_io, true);
                    check(source.discard_checked() && b_root.valid() && b_io->live() == 2 &&
                              b_io->unlinks == b_unlinks + 3 && b_io->closes == b_closes + 3 &&
                              b.inventory() == b_baseline,
                          "source checked completion leaked or damaged root");
                    auto next_b = begin(b_root);
                    check(next_b.valid(), "source admission not released");
                    auto busy_again = begin(b_root);
                    exact(busy_again.error(), RootErrorCode::WrongState);
                    const auto after_release = b_io->used;
                    terminal(source, *b_io);
                    check(b_io->used == after_release && next_b.valid() && next_b.discard_checked(),
                          "terminal source released admission twice");
                    ++validated;
                    ++injections;
                    std::cout << "MOVE_INJECTION " << Campaign << " variant=" << variant
                              << " planned=" << ordinal << " reached=1 validated=1 expected_code="
                              << static_cast<int>(RootErrorCode::IoFailure)
                              << " expected_op=" << static_cast<int>(expected.operation)
                              << " expected_role=" << static_cast<int>(expected.role)
                              << " expected_stage=" << static_cast<int>(expected.stage)
                              << " expected_errno=" << EIO
                              << " actual_code=" << static_cast<int>(first.code)
                              << " actual_op=" << static_cast<int>(first.operation)
                              << " actual_role=" << static_cast<int>(first.role)
                              << " actual_stage=" << static_cast<int>(first.stage)
                              << " actual_errno=" << first.system_errno
                              << " target_removed=" << removed << " target_fds=" << a_io->live()
                              << " source_context=unchanged source_busy=local"
                                 " source_recheck=passed source_release=once "
                                 "source_reacquire=passed allocation_attempts=0\n";
                }
                check(b.inventory() == b_baseline && b_io->reads == 0 && b_io->writes == 0 &&
                          b_io->syncs == 0 && a_io->reads == 0 && a_io->writes == 0 &&
                          a_io->syncs == 0,
                      "move campaign content IO/publication");
                b_root = RootLease{};
                check(b_io->live() == 0 && b_io->opens == b_io->closes && b_io->tracking_ok &&
                          b_io->flags_ok,
                      "incoming root descriptor cleanup");
                contention(b, *b_io, false);
            }
            check(b.inventory() == b_baseline, "source protected inventory after destruction");
        };
        scenario(std::string(Campaign) + " discover " + variant,
                 [&](auto& a, auto& io, auto& root) { run(a, io, root, 0); });
        for (std::size_t ordinal = 1; ordinal <= count; ++ordinal)
            scenario(std::string(Campaign) + " " + variant + " hook " + std::to_string(ordinal),
                     [&](auto& a, auto& io, auto& root) { run(a, io, root, ordinal); });
        check(count == executed && count == reached && count == validated &&
                  stages[0] + stages[1] + stages[2] == count,
              "move planned/reached/validated mismatch");
        total_planned += count;
        total_reached += reached;
        total_validated += validated;
        std::cout << "MOVE_VARIANT " << Campaign << " variant=" << variant
                  << " before=" << stages[0] << " call=" << stages[1] << " after=" << stages[2]
                  << " planned=" << count << " executed=" << executed << " reached=" << reached
                  << " validated=" << validated
                  << " excluded=0 exclusions=none-all-healthy-boundaries-eligible\n";
    }
    scenario(std::string(Campaign) + " closed target control", [](auto&, auto&, auto& root) {
        Fixture b;
        b.files();
        auto io = std::make_shared<Adapter>();
        auto b_root = RootLease::open_existing(b.path, io);
        check(b_root.valid(), "closed-target incoming root");
        io->forbid_reopen = true;
        auto target = begin(root);
        check(target.close_checked(), "closed-target setup");
        auto source = begin(b_root);
        const auto context = move_context(source);
        target = std::move(source);
        same_move_context(target, context);
        check(!source.valid() && target.discard_checked() && root.valid() && b_root.valid(),
              "closed-target transfer");
    });
    scenario(std::string(Campaign) + " failed incoming source control", [](auto&, auto&,
                                                                           auto& root) {
        Fixture b;
        b.files();
        auto io = std::make_shared<Adapter>();
        auto b_root = RootLease::open_existing(b.path, io);
        check(b_root.valid(), "failed-source incoming root");
        io->forbid_reopen = true;
        auto source = begin(b_root, UINT64_MAX);
        const auto first = source.error();
        exact(first, RootErrorCode::GenerationExhausted);
        auto target = begin(root);
        check(target.valid(), "failed-source target setup");
        target = std::move(source);
        check(!target.valid() && target.error() == first && !source.valid() && root.valid(),
              "failed-source error transfer");
        auto next = begin(root);
        check(next.valid() && next.discard_checked(), "failed-source contaminated old target root");
    });
    std::cout << "MOVE_CAMPAIGN " << Campaign << " variants=2 planned=" << total_planned
              << " reached=" << total_reached << " validated=" << total_validated
              << " excluded=0\n";
}

} // namespace
int main() {
    std::array<bool, 512> original_fds{};
    for (std::size_t i = 0; i < original_fds.size(); ++i)
        original_fds[i] = ::fcntl(static_cast<int>(i), F_GETFD) >= 0;
    try {
        positive_and_admission();
        occupancy_and_scripts();
        creates_and_metadata();
        lifetimes_moves_and_closes();
        fault_matrix();
        crash_survival();
        cross_root_move_target_disposal();
        for (std::size_t i = 0; i < original_fds.size(); ++i)
            check(original_fds[i] == (::fcntl(static_cast<int>(i), F_GETFD) >= 0),
                  "process FD inventory changed");
        std::cout << "Pending generation: " << scenarios << " executed scenarios; " << injections
                  << " reached boundary injections; no new-API content IO, fsync, final "
                     "installation or CURRENT write\n";
        return 0;
    } catch (const std::exception& e) {
        allocation_budget = -1;
        std::cerr << "Pending generation FAILED after " << scenarios << " scenarios: " << e.what()
                  << '\n';
        return 1;
    }
}
