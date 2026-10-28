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
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
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
            if (entry->d_name[0] == '.')
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

// Fixed-capacity traces and scripts never allocate inside noexcept hooks. Call
// events are recorded by the actual adapter override, independently of observers.
struct Adapter final : RootIo {
    struct Entry {
        int fd = -1;
        RootRole role = RootRole::Root;
    };
    struct Trace {
        RootIoEvent event{};
        int fd = -1;
        std::size_t count = 0;
        off_t offset = 0;
    };
    struct Answer {
        ssize_t result = 0;
        int error = 0;
    };
    std::array<Entry, 128> descriptors{};
    std::array<Trace, 8192> trace{};
    std::size_t used = 0, target = 0, reached = 0;
    std::size_t creates = 0, randoms = 0, writes = 0, reads = 0, syncs = 0, unlinks = 0, closes = 0;
    std::size_t max_write = 0, max_read = 0;
    std::array<Answer, 8> write_script{}, read_script{}, sync_script{};
    std::size_t write_size = 0, read_size = 0, sync_size = 0;
    std::size_t write_index = 0, read_index = 0, sync_index = 0;
    bool fixed_token = false, ambiguous_create = false, reuse_close = false;
    bool ambiguous_unlink = false;
    int unlink_error = 0;
    int replacement = -1;
    int malformed = 0; // staged held metadata: type, links, size, inode, negative size
    RootRole changed_role = static_cast<RootRole>(99); // fake root/LOCK identity change
    bool changed_named = false;
    std::array<char, 70> last_name{}, ambiguous_name{};
    int staged_fd = -1;
    RootRole active_role = RootRole::Root;
    std::size_t live() const noexcept {
        std::size_t n = 0;
        for (const auto& d : descriptors)
            if (d.fd >= 0)
                ++n;
        return n;
    }
    RootRole role_of(int fd) const noexcept {
        for (const auto& d : descriptors)
            if (d.fd == fd)
                return d.role;
        return active_role;
    }
    void adopt(int fd, RootRole role) noexcept {
        if (fd < 0)
            return;
        for (auto& d : descriptors)
            if (d.fd < 0) {
                d = {fd, role};
                return;
            }
        std::abort();
    }
    bool record(RootIoEvent event, int fd = -1, std::size_t count = 0, off_t offset = 0) noexcept {
        if (used >= trace.size())
            std::abort();
        trace[used++] = {event, fd, count, offset};
        if (target && used == target) {
            ++reached;
            return true;
        }
        return false;
    }
    bool call(RootOperation op, RootRole role, int fd = -1, std::size_t count = 0,
              off_t offset = 0) noexcept {
        if (record({op, role, RootStage::Call, 0, 0}, fd, count, offset)) {
            errno = EIO;
            return false;
        }
        return true;
    }
    void reset_trace() noexcept { used = target = reached = 0; }
    void dump(const char* label) const {
        std::cout << "TRACE " << label << " live=" << live() << " reached=" << reached << '\n';
        for (std::size_t i = 0; i < used; ++i) {
            const auto& t = trace[i];
            std::cout << i + 1 << ':' << static_cast<int>(t.event.operation) << '/'
                      << static_cast<int>(t.event.role) << '/' << static_cast<int>(t.event.stage)
                      << " result=" << t.event.result << " errno=" << t.event.system_errno
                      << " fd=" << t.fd << " count=" << t.count << " offset=" << t.offset << '\n';
        }
    }
    int observe(const RootIoEvent& event) noexcept override {
        if (event.stage == RootStage::Before)
            active_role = event.role;
        return record(event) ? EIO : 0;
    }
    int open(const char* path, int flags) noexcept override {
        if (!call(RootOperation::Open, RootRole::Root))
            return -1;
        const auto fd = RootIo::open(path, flags);
        adopt(fd, RootRole::Root);
        return fd;
    }
    int open_at(int dir, const char* name, int flags) noexcept override {
        if (!call(RootOperation::OpenAt, active_role))
            return -1;
        const auto fd = RootIo::open_at(dir, name, flags);
        adopt(fd, active_role);
        return fd;
    }
    int create_at(int dir, const char* name, int flags, mode_t mode) noexcept override {
        ++creates;
        if (flags != (O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) ||
            mode != 0600)
            std::abort();
        std::strcpy(last_name.data(), name);
        if (!call(RootOperation::CreateAt, active_role))
            return -1;
        const auto fd = RootIo::create_at(dir, name, flags, mode);
        if (ambiguous_create && fd >= 0) {
            ambiguous_create = false;
            std::strcpy(ambiguous_name.data(), name);
            if (::close(fd) != 0)
                std::abort();
            errno = EINTR;
            return -1;
        }
        staged_fd = fd;
        adopt(fd, active_role);
        return fd;
    }
    int random_128(std::array<std::byte, 16>& token) noexcept override {
        if (!call(RootOperation::Random128, active_role))
            return -1;
        token.fill(std::byte{0}); // Zero is legal and tested.
        if (!fixed_token)
            token.back() = static_cast<std::byte>(randoms & 255);
        ++randoms;
        return 0;
    }
    int stat(int fd, struct stat* st) noexcept override {
        const auto role = role_of(fd);
        if (!call(RootOperation::Stat, role, fd))
            return -1;
        const auto result = RootIo::stat(fd, st);
        if (result == 0) {
            if (role == changed_role)
                ++st->st_ino;
            if (fd == staged_fd) {
                switch (malformed) {
                case 1:
                    st->st_mode = S_IFIFO | 0600;
                    break;
                case 2:
                    st->st_nlink = 2;
                    break;
                case 3:
                    ++st->st_size;
                    break;
                case 4:
                    ++st->st_ino;
                    break;
                case 5:
                    st->st_size = -1;
                    break;
                default:
                    break;
                }
            }
        }
        return result;
    }
    int named_stat(int dir, const char* name, struct stat* st, int flags) noexcept override {
        if (!call(RootOperation::NamedStat, active_role))
            return -1;
        const auto result = RootIo::named_stat(dir, name, st, flags);
        if (result == 0 && changed_named && active_role >= RootRole::StagedBase)
            ++st->st_ino;
        return result;
    }
    int lock(int fd, int flags) noexcept override {
        if (!call(RootOperation::Lock, RootRole::LockFile, fd))
            return -1;
        return RootIo::lock(fd, flags);
    }
    ssize_t write_at(int fd, const void* bytes, std::size_t count, off_t offset) noexcept override {
        ++writes;
        max_write = std::max(max_write, count);
        if (!call(RootOperation::WriteAt, role_of(fd), fd, count, offset))
            return -1;
        if (write_index < write_size) {
            const auto a = write_script[write_index++];
            if (a.result <= 0 || static_cast<std::uint64_t>(a.result) > count) {
                errno = a.error;
                return a.result;
            }
            count = static_cast<std::size_t>(a.result);
        }
        return RootIo::write_at(fd, bytes, count, offset);
    }
    ssize_t read(int fd, void* bytes, std::size_t count, off_t offset) noexcept override {
        ++reads;
        max_read = std::max(max_read, count);
        if (!call(RootOperation::Read, role_of(fd), fd, count, offset))
            return -1;
        if (read_index < read_size) {
            const auto a = read_script[read_index++];
            if (a.result <= 0 || static_cast<std::uint64_t>(a.result) > count) {
                errno = a.error;
                return a.result;
            }
            count = static_cast<std::size_t>(a.result);
        }
        return RootIo::read(fd, bytes, count, offset);
    }
    int sync_file(int fd) noexcept override {
        ++syncs;
        if (!call(RootOperation::SyncFile, role_of(fd), fd))
            return -1;
        if (sync_index < sync_size) {
            const auto a = sync_script[sync_index++];
            errno = a.error;
            return static_cast<int>(a.result);
        }
        return RootIo::sync_file(fd);
    }
    int unlink_at(int dir, const char* name) noexcept override {
        ++unlinks;
        if (!call(RootOperation::UnlinkAt, active_role))
            return -1;
        if (ambiguous_unlink) {
            ambiguous_unlink = false;
            if (RootIo::unlink_at(dir, name) != 0)
                std::abort();
            errno = EINTR;
            return -1;
        }
        if (unlink_error) {
            errno = unlink_error;
            return -1;
        }
        return RootIo::unlink_at(dir, name);
    }
    int close(int fd) noexcept override {
        ++closes;
        const auto role = role_of(fd);
        for (auto& d : descriptors)
            if (d.fd == fd) {
                d.fd = -1;
                break;
            }
        const auto allowed = call(RootOperation::Close, role, fd);
        const auto result = RootIo::close(fd); // ALWAYS consume, even injected failure.
        if (reuse_close && role >= RootRole::StagedBase) {
            reuse_close = false;
            replacement = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (replacement != fd)
                std::abort();
            errno = EINTR;
            return -1;
        }
        if (!allowed) {
            errno = EIO;
            return -1;
        }
        return result;
    }
};

std::size_t scenarios = 0;
const std::array<std::byte, 8> small{std::byte{0}, std::byte{255}, std::byte{3}, std::byte{128},
                                     std::byte{0}, std::byte{5},   std::byte{6}, std::byte{7}};
void preserved(const Fixture::Inventory& before, const Fixture::Inventory& after) {
    for (const auto& [name, bytes] : before) {
        const auto it = after.find(name);
        check(it != after.end() && it->second == bytes,
              "selected/LOCK/V1 bytes or membership changed");
    }
}
template <class Fn> void scenario(const std::string& name, Fn fn) {
    Fixture f;
    f.files();
    f.put("legacy.aof", std::string("V1\0journal", 10));
    f.put("generation-1.snap", "V1 snapshot sentinel");
    f.put("legacy.aof.commit", "V1 watermark sentinel");
    const auto before = f.inventory();
    auto io = std::make_shared<Adapter>();
    {
        auto root = RootLease::open_existing(f.path, io);
        check(root.valid() && io->live() == 2, "fixture root open");
        fn(f, io, root);
    }
    check(io->live() == 0, "descriptor ownership leak");
    preserved(before, f.inventory());
    ++scenarios;
    std::cout
        << "SCENARIO " << scenarios << " PASS " << name
        << " preservation=LOCK,CURRENT,base,tail,watermark,V1 exact bytes+membership; live=0\n";
}
StagedFile create(RootLease& root) {
    return StagedFile::create_data(root, ArtifactRole::Base, gen());
}
void terminal(StagedFile& file, RootLease& root, Adapter& io) {
    const auto first = file.error();
    check(first.code != RootErrorCode::None && !root.valid(), "shared failure missing");
    const auto writes = io.writes, syncs = io.syncs, unlinks = io.unlinks;
    check(!file.append(small) && !file.seal_file() && !file.discard_checked(),
          "failed owner mutated");
    check(io.writes == writes && io.syncs == syncs && io.unlinks == unlinks,
          "terminal mutation syscall");
    check(file.error() == first && root.error() == first, "first shared error changed");
    std::cout << "FIRST_ERROR " << static_cast<int>(first.code) << '/'
              << static_cast<int>(first.operation) << '/' << static_cast<int>(first.role) << '/'
              << static_cast<int>(first.stage) << '/' << first.system_errno << '\n';
}

void positive() {
    for (const auto role : {ArtifactRole::Base, ArtifactRole::Tail, ArtifactRole::Watermark}) {
        for (const auto g : {std::uint64_t{1}, UINT64_MAX}) {
            scenario(
                "role/generation " + std::to_string(static_cast<int>(role)) + "/" +
                    std::to_string(g),
                [=](auto& f, auto& io, auto& root) {
                    auto file = StagedFile::create_data(root, role, gen(g));
                    check(file.valid() && file.identity() && file.byte_length() == 0,
                          "empty create");
                    const auto prefix =
                        std::string(role == ArtifactRole::Base ? "base-" : "tail-") +
                        std::to_string(g) +
                        (role == ArtifactRole::Base   ? ".snap.tmp."
                         : role == ArtifactRole::Tail ? ".aof.tmp."
                                                      : ".commit.tmp.");
                    check(std::string(io->last_name.data()) == prefix + std::string(32, '0'),
                          "exact grammar/zero token");
                    const auto length = std::strlen(io->last_name.data());
                    check(length <= 69 &&
                              (role != ArtifactRole::Watermark || g != UINT64_MAX || length == 69),
                          "maximum name length");
                    struct stat st{};
                    check(::lstat((f.path / io->last_name.data()).c_str(), &st) == 0 &&
                              (st.st_mode & 0777) == 0600,
                          "owner-only mode");
                    check((::fcntl(io->staged_fd, F_GETFD) & FD_CLOEXEC) != 0, "close on exec");
                    check(file.append({}) && file.read_exact(0, {}) && file.seal_file(),
                          "empty high-level operations");
                    const auto n = io->used;
                    check(file.seal_file() && io->used > n && io->syncs == 1,
                          "repeat seal revalidates");
                    check(file.discard_checked() && io->unlinks == 1, "healthy discard");
                    check(!std::filesystem::exists(f.path / io->last_name.data()),
                          "discard removes exact name");
                });
        }
    }
    scenario("large binary / multiple appends / exact bounded read",
             [](auto&, auto& io, auto& root) {
                 auto file = create(root);
                 std::vector<std::byte> bytes(196731);
                 for (std::size_t i = 0; i < bytes.size(); ++i)
                     bytes[i] = static_cast<std::byte>(i & 255);
                 check(file.append(small) && file.append(bytes), "multiple append");
                 std::vector<std::byte> output(bytes.size());
                 check(file.read_exact(small.size(), output) && output == bytes, "binary readback");
                 check(file.byte_length() == small.size() + bytes.size() &&
                           io->max_write == 65536 && io->max_read == 65536,
                       "64KiB bounds");
                 check(file.seal_file(), "file sync");
                 io->dump("binary");
             });
    scenario("short write/read and actual EINTR retries", [](auto&, auto& io, auto& root) {
        auto file = create(root);
        io->write_script[0] = {-1, EINTR};
        io->write_script[1] = {2, 0};
        io->write_size = 2;
        check(file.append(small) && io->writes == 3, "short write/EINTR");
        io->read_script[0] = {-1, EINTR};
        io->read_script[1] = {3, 0};
        io->read_size = 2;
        std::array<std::byte, 8> output{};
        check(file.read_exact(0, output) && output == small && io->reads == 3, "short read/EINTR");
        io->sync_script[0] = {-1, EINTR};
        io->sync_size = 1;
        check(file.seal_file() && io->syncs == 2, "fsync EINTR");
    });
    scenario("healthy destructor cleanup", [](auto& f, auto& io, auto& root) {
        {
            auto file = create(root);
            check(file.append(small), "append");
        }
        check(io->unlinks == 1 && !std::filesystem::exists(f.path / io->last_name.data()),
              "destructor cleanup");
    });
    scenario("explicit close retains temporary and shared lock", [](auto& f, auto& io, auto& root) {
        auto file = create(root);
        check(file.close_checked(), "close");
        check(std::filesystem::exists(f.path / io->last_name.data()) && io->unlinks == 0,
              "closed name retained");
        check(!file.valid() && !file.recheck() && !file.append({}) && !file.seal_file() &&
                  !file.read_exact(0, {}) && !file.discard_checked() && !file.close_checked(),
              "closed operations");
        check(!root.close_checked() && root.error().code == RootErrorCode::DependentsAlive,
              "closed dependent retains lease");
    });
    scenario("moves, self move, assignment disposal", [](auto& f, auto& io, auto& root) {
        auto first = create(root);
        check(first.append(small), "append");
        auto moved = std::move(first);
        check(!first.valid() && !first.recheck() && !first.append({}) && !first.read_exact(0, {}) &&
                  !first.seal_file() && !first.discard_checked() && !first.close_checked(),
              "moved-from operations");
        auto* self = &moved;
        moved = std::move(*self);
        check(moved.valid() && moved.byte_length() == 8, "self move");
        auto target = create(root);
        const std::string old = io->last_name.data();
        target = std::move(moved);
        check(!std::filesystem::exists(f.path / old) && target.valid() && !moved.valid(),
              "assignment disposal");
    });
    scenario("root destruction / pins and staging share LOCK / child contention", [](auto& f,
                                                                                     auto& io,
                                                                                     auto& root) {
        auto pin = root.pin_current();
        auto file = create(root);
        root = RootLease{};
        check(file.recheck() && pin.recheck(), "surviving dependents");
        const auto contention = [&](bool locked) {
            const auto pid = ::fork();
            check(pid >= 0, "fork");
            if (pid == 0) {
                for (const auto& d : io->descriptors)
                    if (d.fd >= 0)
                        (void)::close(d.fd);
                const auto fd =
                    ::open((f.path / "LOCK").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (fd < 0)
                    ::_exit(2);
                errno = 0;
                const auto result = ::flock(fd, LOCK_EX | LOCK_NB);
                const auto saved = errno;
                (void)::close(fd);
                ::_exit(locked ? (result == -1 && (saved == EWOULDBLOCK || saved == EAGAIN) ? 0 : 3)
                               : (result == 0 ? 0 : 4));
            }
            int status = 0;
            check(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                      WEXITSTATUS(status) == 0,
                  "independent child LOCK result");
        };
        contention(true);
        file = StagedFile{};
        contention(true);
        pin = PinnedFile{};
        contention(false);
    });
    scenario("default constructed operations", [](auto&, auto&, auto&) {
        StagedFile file;
        check(!file.valid() && !file.identity() && file.byte_length() == 0 && !file.recheck() &&
                  !file.append({}) && !file.read_exact(0, {}) && !file.seal_file() &&
                  !file.discard_checked() && !file.close_checked(),
              "default operations");
    });
    scenario("real OS cryptographic entropy", [](auto& f, auto&, auto& root) {
        root = RootLease{};
        auto actual = RootLease::open_existing(f.path);
        auto file = create(actual);
        check(file.valid() && file.append(small) && file.seal_file() && file.discard_checked(),
              "real getentropy and fsync");
    });
}

void adversarial() {
    scenario("invalid role rejects before mutation", [](auto&, auto& io, auto& root) {
        auto file = StagedFile::create_data(root, static_cast<ArtifactRole>(99), gen());
        check(!file.valid() && file.error().code == RootErrorCode::InvalidName &&
                  io->creates == 0 && io->randoms == 0,
              "invalid role");
        terminal(file, root, *io);
    });
    scenario("invalid generation construction and parse", [](auto&, auto& io, auto&) {
        try {
            (void)gen(0);
            check(false, "zero generation accepted");
        } catch (const FormatError&) {
        }
        for (const auto* text : {"0", "01", "-1", "+1", "18446744073709551616", "1/2", ""}) {
            try {
                (void)parse_generation(text);
                check(false, "invalid generation parse");
            } catch (const FormatError&) {
            }
        }
        check(io->creates == 0 && io->randoms == 0, "invalid generation mutated");
    });
    for (int kind = 0; kind < 5; ++kind) {
        scenario("actual EEXIST preserves collision kind " + std::to_string(kind),
                 [=](auto& f, auto& io, auto& root) {
                     const std::string name = "base-1.snap.tmp." + std::string(32, '0');
                     const auto p = f.path / name;
                     switch (kind) {
                     case 0:
                         f.put(name.c_str(), "collision bytes");
                         break;
                     case 1:
                         check(::symlink("CURRENT", p.c_str()) == 0, "collision symlink");
                         break;
                     case 2:
                         check(::mkfifo(p.c_str(), 0600) == 0, "collision FIFO");
                         break;
                     case 3:
                         check(::mkdir(p.c_str(), 0700) == 0, "collision directory");
                         break;
                     case 4:
                         f.put("unowned-source", "unowned hardlink bytes");
                         check(::link((f.path / "unowned-source").c_str(), p.c_str()) == 0,
                               "collision hardlink");
                         break;
                     }
                     const auto before = f.inventory();
                     auto file = create(root);
                     check(file.valid() && io->creates == 2 && io->randoms == 2 &&
                               std::string(io->last_name.data()) != name,
                           "collision fresh token");
                     check(file.discard_checked(), "discard own temporary");
                     check(before == f.inventory(), "unowned collision changed");
                 });
    }
    scenario("collision limit exactly 64", [](auto& f, auto& io, auto& root) {
        const std::string name = "base-1.snap.tmp." + std::string(32, '0');
        f.put(name.c_str(), "retain collision");
        const auto before = f.inventory();
        io->fixed_token = true;
        auto file = create(root);
        check(!file.valid() && file.error().code == RootErrorCode::CollisionLimit &&
                  io->creates == 64 && io->randoms == 64,
              "collision limit");
        terminal(file, root, *io);
        check(before == f.inventory(), "collision exhaustion preservation");
    });
    scenario("actual create EINTR retains ambiguous name and uses fresh token",
             [](auto& f, auto& io, auto& root) {
                 io->ambiguous_create = true;
                 auto file = create(root);
                 check(file.valid() && io->creates == 2 && io->randoms == 2, "EINTR fresh create");
                 check(std::strcmp(io->ambiguous_name.data(), io->last_name.data()) != 0,
                       "fresh token after EINTR");
                 check(std::filesystem::exists(f.path / io->ambiguous_name.data()),
                       "ambiguous name preserved");
                 check(file.discard_checked() &&
                           std::filesystem::exists(f.path / io->ambiguous_name.data()),
                       "no ambiguous cleanup");
             });
    for (int mismatch = 1; mismatch <= 5; ++mismatch) {
        scenario("newly created held metadata mismatch " + std::to_string(mismatch),
                 [=](auto& f, auto& io, auto& root) {
                     io->malformed = mismatch;
                     auto file = create(root);
                     check(!file.valid() && io->live() == 2 && io->unlinks == 0,
                           "created mismatch closes without cleanup");
                     check(std::filesystem::exists(f.path / io->last_name.data()),
                           "failed validation retains temp");
                     terminal(file, root, *io);
                 });
    }
    for (int change = 0; change < 4; ++change) {
        scenario("post-create external inode/extent change " + std::to_string(change),
                 [=](auto& f, auto& io, auto& root) {
                     auto file = create(root);
                     check(file.append(small), "append");
                     const auto p = f.path / io->last_name.data();
                     if (change == 0) {
                         f.remove(io->last_name.data());
                         f.put(io->last_name.data(), "replacement");
                     }
                     if (change == 1)
                         check(::truncate(p.c_str(), 0) == 0, "external truncate");
                     if (change == 2)
                         check(::truncate(p.c_str(), 9) == 0, "external extend");
                     if (change == 3)
                         io->changed_named = true;
                     const auto before = f.inventory();
                     check(!file.read_exact(0, {}), "empty read must recheck identities");
                     terminal(file, root, *io);
                     check(before == f.inventory(), "uncertain staging names retained");
                 });
    }
    for (const auto role : {RootRole::Root, RootRole::LockFile}) {
        scenario("root/LOCK held inode substitution " + std::to_string(static_cast<int>(role)),
                 [=](auto&, auto& io, auto& root) {
                     auto file = create(root);
                     io->changed_role = role;
                     check(!file.append({}), "root/LOCK check before empty append");
                     terminal(file, root, *io);
                 });
    }
    scenario("named LOCK replacement preserves selected artifacts",
             [](auto& f, auto& io, auto& root) {
                 auto file = create(root);
                 check(::rename((f.path / "LOCK").c_str(), (f.path / "held-lock").c_str()) == 0,
                       "save lock inode");
                 f.put("LOCK", "");
                 check(!file.recheck(), "named LOCK substitution");
                 terminal(file, root, *io);
                 f.remove("LOCK");
                 check(::rename((f.path / "held-lock").c_str(), (f.path / "LOCK").c_str()) == 0,
                       "restore harness LOCK");
             });
    scenario("root directory inode replacement observed through held directory",
             [](auto& f, auto& io, auto& root) {
                 auto file = create(root);
                 // Directory rename does not change held identity under the trusted-root
                 // contract. A replacement of held identity is injected above; no claim
                 // is made to police hostile ancestor/path races.
                 const auto moved = std::filesystem::path(f.path.string() + "-held");
                 check(::rename(f.path.c_str(), moved.c_str()) == 0, "move harness directory");
                 check(::mkdir(f.path.c_str(), 0700) == 0, "replacement harness directory");
                 check(file.recheck(), "held root remains authority");
                 check(::rmdir(f.path.c_str()) == 0 && ::rename(moved.c_str(), f.path.c_str()) == 0,
                       "restore harness root");
                 check(file.discard_checked() && io->unlinks == 1, "held-root cleanup");
             });
    for (const auto offset :
         {std::uint64_t{9}, static_cast<std::uint64_t>(INT64_MAX), UINT64_MAX}) {
        scenario("subtraction bounds/native offset " + std::to_string(offset),
                 [=](auto&, auto& io, auto& root) {
                     auto file = create(root);
                     check(file.append(small), "append");
                     std::array<std::byte, 1> output{};
                     check(!file.read_exact(offset, output) && io->reads == 0,
                           "offset rejected before narrowing");
                     check(file.error().code == RootErrorCode::InvalidLength, "bounds diagnostic");
                     terminal(file, root, *io);
                 });
    }
    scenario("read span exceeds remaining extent", [](auto&, auto& io, auto& root) {
        auto file = create(root);
        check(file.append(small), "append");
        std::array<std::byte, 8> out{};
        check(!file.read_exact(1, out) && io->reads == 0, "subtraction-first remaining length");
        terminal(file, root, *io);
    });
    for (const auto answer : {Adapter::Answer{0, 0}, Adapter::Answer{-2, EIO},
                              Adapter::Answer{9, 0}, Adapter::Answer{-1, 0}}) {
        scenario("invalid write count " + std::to_string(answer.result) + "/" +
                     std::to_string(answer.error),
                 [=](auto&, auto& io, auto& root) {
                     auto file = create(root);
                     io->write_script[0] = answer;
                     io->write_size = 1;
                     check(!file.append(small) && file.byte_length() == 0,
                           "invalid write rejected");
                     terminal(file, root, *io);
                 });
    }
    for (const auto err : {EIO, ENOSPC, EFBIG}) {
        scenario(
            "partial write then errno " + std::to_string(err), [=](auto& f, auto& io, auto& root) {
                auto file = create(root);
                io->write_script[0] = {3, 0};
                io->write_script[1] = {-1, err};
                io->write_size = 2;
                check(!file.append(small) && file.byte_length() == 3 &&
                          file.error().system_errno == err,
                      "partial extent/error");
                struct stat st{};
                check(::lstat((f.path / io->last_name.data()).c_str(), &st) == 0 && st.st_size == 3,
                      "partial file retained");
                terminal(file, root, *io);
            });
    }
    for (const auto answer :
         {Adapter::Answer{0, 0}, Adapter::Answer{-2, EIO}, Adapter::Answer{9, 0},
          Adapter::Answer{-1, 0}, Adapter::Answer{-1, EIO}}) {
        scenario("invalid/truncated read " + std::to_string(answer.result) + "/" +
                     std::to_string(answer.error),
                 [=](auto&, auto& io, auto& root) {
                     auto file = create(root);
                     check(file.append(small), "append");
                     io->read_script[0] = answer;
                     io->read_size = 1;
                     std::array<std::byte, 8> out{};
                     check(!file.read_exact(0, out), "invalid read rejected");
                     terminal(file, root, *io);
                 });
    }
    scenario("read partial fill then truncation", [](auto&, auto& io, auto& root) {
        auto file = create(root);
        check(file.append(small), "append");
        io->read_script[0] = {3, 0};
        io->read_script[1] = {0, 0};
        io->read_size = 2;
        std::array<std::byte, 8> out{};
        out.fill(std::byte{42});
        check(!file.read_exact(0, out) && std::equal(out.begin(), out.begin() + 3, small.begin()) &&
                  out[3] == std::byte{42},
              "partial caller buffer");
        check(file.error().code == RootErrorCode::Truncated, "read truncation code");
        terminal(file, root, *io);
    });
    for (const auto err : {EIO, ENOSPC, EINVAL}) {
        scenario("fsync failure " + std::to_string(err), [=](auto&, auto& io, auto& root) {
            auto file = create(root);
            io->sync_script[0] = {-1, err};
            io->sync_size = 1;
            check(!file.seal_file() && io->syncs == 1 && file.error().system_errno == err,
                  "fsync failure no retry");
            terminal(file, root, *io);
        });
    }
    scenario("repeat seal detects extent substitution", [](auto&, auto& io, auto& root) {
        auto file = create(root);
        check(file.seal_file(), "first seal");
        io->malformed = 3;
        check(!file.seal_file() && io->syncs == 1, "repeat seal identity check");
        terminal(file, root, *io);
    });
    scenario("append after seal terminal wrong-state", [](auto&, auto& io, auto& root) {
        auto file = create(root);
        check(file.seal_file(), "seal");
        check(!file.append({}) && file.error().code == RootErrorCode::WrongState && io->writes == 0,
              "sealed append");
        terminal(file, root, *io);
    });
    scenario("close failure and descriptor-number reuse", [](auto&, auto& io, auto& root) {
        {
            auto file = create(root);
            io->reuse_close = true;
            check(!file.close_checked() && file.error().system_errno == EINTR,
                  "close EINTR terminal");
            check(io->replacement >= 0 && ::fcntl(io->replacement, F_GETFD) >= 0,
                  "reused descriptor live");
            terminal(file, root, *io);
        }
        check(::fcntl(io->replacement, F_GETFD) >= 0, "destructor must not close reused number");
        check(::close(io->replacement) == 0, "harness closes replacement");
        io->replacement = -1;
    });
    scenario(
        "move assignment cannot restore failed target health / cross-root isolation",
        [](auto&, auto& io, auto& root) {
            Fixture other;
            other.files();
            const auto before = other.inventory();
            auto other_io = std::make_shared<Adapter>();
            {
                auto other_root = RootLease::open_existing(other.path, other_io);
                auto incoming = create(other_root);
                auto target = create(root);
                io->malformed = 4;
                target = std::move(incoming);
                check(!target.valid() && target.error().code == RootErrorCode::IdentityChanged &&
                          !root.valid(),
                      "disposal failure preserved");
                check(other_root.valid() && other_io->unlinks == 0, "independent lease health");
                check(!target.append(small) && !target.discard_checked(),
                      "failed moved target cannot mutate");
            }
            check(other_io->live() == 0, "other-root descriptors closed");
            preserved(before, other.inventory());
        });
    scenario("unlink EINTR preserves uncertainty without retry", [](auto& f, auto& io, auto& root) {
        auto file = create(root);
        io->unlink_error = EINTR;
        check(!file.discard_checked() && io->unlinks == 1 && file.error().system_errno == EINTR,
              "unlink EINTR once");
        check(std::filesystem::exists(f.path / io->last_name.data()), "uncertain name retained");
        terminal(file, root, *io);
        check(io->unlinks == 1, "no unlink retry");
    });
    scenario("successful actual unlink with ambiguous reported EINTR", [](auto& f, auto& io,
                                                                          auto& root) {
        auto file = create(root);
        io->ambiguous_unlink = true;
        check(!file.discard_checked() && io->unlinks == 1, "ambiguous unlink once");
        check(!std::filesystem::exists(f.path / io->last_name.data()), "actual unlink completed");
        terminal(file, root, *io);
        check(io->unlinks == 1, "no ambiguous unlink retry");
    });
    scenario("reached shared-state allocation failure propagates into staging",
             [](auto& f, auto& io, auto& root) {
                 root = RootLease{};
                 const auto failures = allocation_rejections;
                 allocation_budget = 0;
                 auto failed = RootLease::open_existing(f.path, io);
                 auto file = create(failed);
                 allocation_budget = -1;
                 check(allocation_rejections == failures + 1 && !file.valid() &&
                           file.error().code == RootErrorCode::ResourceLimit,
                       "reached allocation failure");
                 check(io->live() == 0 && io->creates == 0, "allocation rejection before creation");
                 std::cout << "ALLOCATION reached RootState make_shared failure=1\n";
             });
    scenario("staging path allocates nothing at budget zero", [](auto&, auto& io, auto& root) {
        const auto allocations_before = guarded_allocations;
        allocation_budget = 0;
        auto file = create(root);
        const auto a = file.valid() && file.append(small);
        std::array<std::byte, 8> out{};
        const auto b =
            file.read_exact(0, out) && out == small && file.seal_file() && file.seal_file();
        auto moved = std::move(file);
        const auto c = moved.discard_checked();
        allocation_budget = -1;
        check(a && b && c && io->live() == 2 && guarded_allocations == allocations_before,
              "unexpected staging allocation");
        std::cout << "ALLOCATION staging lifecycle: zero reached allocation sites\n";
    });
}

// Discover exact reached boundaries on a healthy path, then independently
// inject EACH discovered boundary. No configured-but-unreached fault is counted.
enum class Action { Create, Append, Read, Seal, RepeatSeal, Discard, Close };
StagedFile prepare(Action action, RootLease& root) {
    if (action == Action::Create)
        return {};
    auto file = create(root);
    check(file.valid(), "matrix setup create");
    if (action == Action::Read)
        check(file.append(small), "matrix setup append");
    if (action == Action::RepeatSeal)
        check(file.seal_file(), "matrix setup seal");
    return file;
}
bool invoke(Action action, StagedFile& file, RootLease& root) {
    std::array<std::byte, 8> output{};
    switch (action) {
    case Action::Create:
        file = create(root);
        return file.valid();
    case Action::Append:
        return file.append(small);
    case Action::Read:
        return file.read_exact(0, output);
    case Action::Seal:
    case Action::RepeatSeal:
        return file.seal_file();
    case Action::Discard:
        return file.discard_checked();
    case Action::Close:
        return file.close_checked();
    }
    return false;
}
void boundary_matrix() {
    for (const auto action : {Action::Create, Action::Append, Action::Read, Action::Seal,
                              Action::RepeatSeal, Action::Discard, Action::Close}) {
        std::size_t boundaries = 0;
        std::array<Adapter::Trace, 512> expected{};
        scenario("discover boundaries action " + std::to_string(static_cast<int>(action)),
                 [&](auto&, auto& io, auto& root) {
                     auto file = prepare(action, root);
                     io->reset_trace();
                     check(invoke(action, file, root), "matrix healthy operation");
                     boundaries = io->used;
                     check(boundaries <= expected.size(), "trace capacity");
                     std::copy_n(io->trace.begin(), boundaries, expected.begin());
                     io->dump("healthy discovery");
                 });
        for (std::size_t point = 1; point <= boundaries; ++point) {
            scenario("reached fault action " + std::to_string(static_cast<int>(action)) +
                         " boundary " + std::to_string(point),
                     [&](auto& f, auto& io, auto& root) {
                         auto file = prepare(action, root);
                         io->reset_trace();
                         io->target = point;
                         check(!invoke(action, file, root) && io->reached == 1,
                               "fault must reach and reject");
                         const auto& reached = io->trace[point - 1].event;
                         const auto& planned = expected[point - 1].event;
                         check(reached.operation == planned.operation &&
                                   reached.role == planned.role && reached.stage == planned.stage,
                               "deterministic exact fault identity");
                         const auto error = file.error();
                         check(error.operation == reached.operation && error.role == reached.role &&
                                   error.stage == reached.stage,
                               "first error boundary identity");
                         const auto op = reached.operation;
                         if (op == RootOperation::WriteAt && reached.stage == RootStage::After) {
                             check(file.byte_length() == 8 && io->writes == 1,
                                   "progress recorded before After");
                         }
                         if (op == RootOperation::UnlinkAt && reached.stage == RootStage::After) {
                             check(io->unlinks == 1 &&
                                       !std::filesystem::exists(f.path / io->last_name.data()),
                                   "unlink recorded before After");
                         }
                         if (op == RootOperation::CreateAt && reached.stage == RootStage::After) {
                             check(io->live() == 2 &&
                                       std::filesystem::exists(f.path / io->last_name.data()),
                                   "created fd adopted before After and retained name");
                         }
                         terminal(file, root, *io);
                         io->dump("reached injection");
                     });
        }
    }
}
} // namespace
int main() {
    try {
        positive();
        adversarial();
        boundary_matrix();
        std::cout << "PASS " << scenarios
                  << " executed staging scenarios; every counted boundary injection reached.\n";
        return 0;
    } catch (const std::exception& error) {
        allocation_budget = -1;
        std::cerr << "FAIL after " << scenarios << " scenarios: " << error.what() << '\n';
        return 1;
    }
}
