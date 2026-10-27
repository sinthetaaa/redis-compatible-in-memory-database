#include "redisdb/persistence/V2Root.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
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
}
void* operator new(std::size_t size) {
    if (allocation_budget == 0)
        throw std::bad_alloc{};
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
std::size_t positive_cases = 0, rejection_cases = 0, fault_cases = 0, allocation_failures = 0;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
GenerationId gen(std::uint64_t n = 1) { return GenerationId{n}; }

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

struct Adapter final : RootIo {
    struct Descriptor {
        int fd = -1;
        RootRole role = RootRole::Root;
    };
    std::array<Descriptor, 32> descriptors{};
    std::array<RootIoEvent, 2048> events{};
    std::size_t event_count = 0, opens = 0, closes = 0, reads = 0, max_request = 0;
    RootRole active = RootRole::Root;
    RootOperation fault_op = RootOperation::None;
    RootRole fault_role = RootRole::Root;
    RootStage fault_stage = RootStage::Call;
    int fault_errno = EIO, hits = 0, occurrence = 1;
    bool armed = false, flags_ok = true, tracking_ok = true;
    RootOperation invalid_result = RootOperation::None;
    bool change_after_read = false;
    const char* truncate_on_read = nullptr;
    const char* reuse_path = nullptr;
    int reused_fd = -1;
    std::size_t short_limit = SIZE_MAX;
    enum class ReadMode {
        Normal,
        InterruptOnce,
        Zero,
        Oversized,
        InvalidNegative
    } read_mode = ReadMode::Normal;
    enum class StatEdit {
        None,
        Negative,
        Maximum,
        Device,
        Inode,
        Links,
        Type,
        Length
    } stat_edit = StatEdit::None;
    RootRole edit_role = RootRole::Current;
    RootOperation edit_op = RootOperation::Stat;
    std::uint64_t read_offset = 0;
    void arm(RootOperation op, RootRole role, RootStage stage = RootStage::Call, int err = EIO) {
        fault_op = op;
        fault_role = role;
        fault_stage = stage;
        fault_errno = err;
        hits = 0;
        occurrence = 1;
        armed = true;
    }
    bool trip(RootOperation op, RootRole role, RootStage stage = RootStage::Call) noexcept {
        if (armed && op == fault_op && role == fault_role && stage == fault_stage &&
            ++hits == occurrence) {
            errno = fault_errno;
            return true;
        }
        return false;
    }
    RootRole role_for(int fd) const noexcept {
        for (const auto& d : descriptors)
            if (d.fd == fd)
                return d.role;
        return RootRole::Root;
    }
    void adopted(int fd) noexcept {
        if (fd < 0)
            return;
        ++opens;
        flags_ok = flags_ok && (::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0 &&
                   (::fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY;
        for (auto& d : descriptors)
            if (d.fd < 0) {
                d = {fd, active};
                return;
            }
        tracking_ok = false;
    }
    std::size_t live() const noexcept {
        return static_cast<std::size_t>(std::count_if(descriptors.begin(), descriptors.end(),
                                                      [](auto d) { return d.fd >= 0; }));
    }
    int open(const char* p, int flags) noexcept override {
        flags_ok = flags_ok && (flags & (O_CREAT | O_TRUNC | O_APPEND)) == 0 &&
                   (flags & (O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) ==
                       (O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (trip(RootOperation::Open, active))
            return -1;
        if (invalid_result == RootOperation::Open)
            return -2;
        const auto fd = RootIo::open(p, flags);
        adopted(fd);
        return fd;
    }
    int open_at(int directory, const char* name, int flags) noexcept override {
        flags_ok = flags_ok && (flags & (O_CREAT | O_TRUNC | O_APPEND)) == 0 &&
                   (flags & (O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)) ==
                       (O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK) &&
                   std::strchr(name, '/') == nullptr;
        if (trip(RootOperation::OpenAt, active))
            return -1;
        if (invalid_result == RootOperation::OpenAt)
            return -2;
        const auto fd = RootIo::open_at(directory, name, flags);
        adopted(fd);
        return fd;
    }
    void edit(struct stat* st, RootRole role, RootOperation op) const noexcept {
        if (role != edit_role || op != edit_op)
            return;
        switch (stat_edit) {
        case StatEdit::Negative:
            st->st_size = -1;
            break;
        case StatEdit::Maximum:
            st->st_size = std::numeric_limits<off_t>::max();
            break;
        case StatEdit::Device:
            ++st->st_dev;
            break;
        case StatEdit::Inode:
            ++st->st_ino;
            break;
        case StatEdit::Links:
            st->st_nlink = 2;
            break;
        case StatEdit::Type:
            st->st_mode = role == RootRole::Root ? S_IFREG : S_IFDIR;
            break;
        case StatEdit::Length:
            ++st->st_size;
            break;
        case StatEdit::None:
            break;
        }
    }
    int stat(int fd, struct stat* st) noexcept override {
        const auto role = role_for(fd);
        if (trip(RootOperation::Stat, role))
            return -1;
        if (invalid_result == RootOperation::Stat)
            return 1;
        const auto n = RootIo::stat(fd, st);
        if (!n)
            edit(st, role, RootOperation::Stat);
        return n;
    }
    int named_stat(int fd, const char* name, struct stat* st, int flags) noexcept override {
        flags_ok = flags_ok && flags == AT_SYMLINK_NOFOLLOW;
        if (trip(RootOperation::NamedStat, active))
            return -1;
        if (invalid_result == RootOperation::NamedStat)
            return 2;
        const auto n = RootIo::named_stat(fd, name, st, flags);
        if (!n)
            edit(st, active, RootOperation::NamedStat);
        return n;
    }
    int lock(int fd, int flags) noexcept override {
        flags_ok = flags_ok && flags == (LOCK_EX | LOCK_NB);
        if (trip(RootOperation::Lock, active))
            return -1;
        if (invalid_result == RootOperation::Lock)
            return 2;
        return RootIo::lock(fd, flags);
    }
    ssize_t read(int fd, void* p, std::size_t count, off_t offset) noexcept override {
        ++reads;
        max_request = std::max(max_request, count);
        read_offset = static_cast<std::uint64_t>(offset);
        if (trip(RootOperation::Read, active))
            return -1;
        if (read_mode == ReadMode::InterruptOnce) {
            read_mode = ReadMode::Normal;
            errno = EINTR;
            return -1;
        }
        if (read_mode == ReadMode::Zero)
            return 0;
        if (read_mode == ReadMode::Oversized)
            return static_cast<ssize_t>(count + 1);
        if (read_mode == ReadMode::InvalidNegative)
            return -2;
        const auto result = RootIo::read(fd, p, std::min(count, short_limit), offset);
        if (change_after_read)
            stat_edit = StatEdit::Length;
        return result;
    }
    int close(int fd) noexcept override {
        const auto role = role_for(fd);
        bool found = false;
        for (auto& d : descriptors)
            if (d.fd == fd) {
                d.fd = -1;
                found = true;
                break;
            }
        tracking_ok = tracking_ok && found;
        ++closes;
        const auto n = RootIo::close(fd); // Always consumes the REAL FD before reporting a fault.
        if (trip(RootOperation::Close, role)) {
            const auto saved = errno;
            if (reuse_path) {
                reused_fd = ::open(reuse_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                tracking_ok = tracking_ok && reused_fd == fd;
            }
            errno = saved;
            return -1;
        }
        if (invalid_result == RootOperation::Close)
            return 2;
        return n;
    }
    int observe(const RootIoEvent& event) noexcept override {
        active = event.role;
        // Retain a bounded trace prefix; counters and FD accounting continue
        // through arbitrarily many short reads without test-side growth.
        if (event_count < events.size())
            events[event_count++] = event;
        if (truncate_on_read && event.operation == RootOperation::Read &&
            event.stage == RootStage::Before) {
            tracking_ok = tracking_ok && ::truncate(truncate_on_read, 0) == 0;
            truncate_on_read = nullptr;
        }
        return trip(event.operation, event.role, event.stage) ? fault_errno : 0;
    }
    void clean() const {
        check(live() == 0 && opens == closes && tracking_ok && flags_ok, "FD/flag accounting");
    }
};

void contender(const Fixture& f, bool should_acquire) {
    int pipefd[2];
    check(::pipe(pipefd) == 0, "contender pipe");
    const auto child = ::fork();
    check(child >= 0, "contender fork");
    if (child == 0) {
        (void)::close(pipefd[0]);
        // A NEW open/openat inside open_existing: never try to flock an inherited
        // description. Inherited copies do not substitute for this independent FD.
        const auto owner = RootLease::open_existing(f.path);
        const bool ok =
            should_acquire ? owner.valid()
                           : !owner.valid() && owner.error().code == RootErrorCode::LockUnavailable;
        const char status = ok ? '!' : '?';
        if (::write(pipefd[1], &status, 1) != 1)
            ::_exit(1);
        ::_exit(ok ? 0 : 1);
    }
    (void)::close(pipefd[1]);
    pollfd ready{pipefd[0], POLLIN, 0};
    char status = 0;
    const bool notified = ::poll(&ready, 1, 5000) > 0 && ::read(pipefd[0], &status, 1) == 1;
    (void)::close(pipefd[0]);
    if (!notified)
        (void)::kill(child, SIGKILL);
    int outcome = 0;
    check(::waitpid(child, &outcome, 0) == child, "contender wait");
    check(notified && status == '!' && WIFEXITED(outcome) && WEXITSTATUS(outcome) == 0,
          "independent flock contender");
}

void basics_and_lifetimes() {
    Fixture f;
    f.files();
    for (auto name : {"base-18446744073709551615.snap", "tail-18446744073709551615.aof",
                      "tail-18446744073709551615.commit"})
        f.put(name, std::string(std::strstr(name, ".snap")     ? 204
                                : std::strstr(name, ".commit") ? 112
                                                               : 96,
                                'x'));
    const auto before = f.inventory();
    auto io = std::make_shared<Adapter>();
    {
        auto root = RootLease::open_existing(f.path, io);
        check(root.valid(), "existing root");
        ++positive_cases;
        contender(f, false);
        ++positive_cases;
        auto current = root.pin_current();
        check(current.valid() && current.identity()->byte_length == 112, "current pin");
        ++positive_cases;
        for (auto id : {gen(), gen(UINT64_MAX)})
            for (auto role : {ArtifactRole::Base, ArtifactRole::Tail, ArtifactRole::Watermark}) {
                auto file = root.pin_generation_file(role, id);
                check(file.valid(), "derived pin");
                for (int n = 0; n < 5; ++n)
                    check(file.recheck(), "repeated identity");
                ++positive_cases;
            }
        std::array<std::byte, 112> bytes{};
        check(
            current.read_exact(0, bytes) &&
                std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == std::byte{'?'}; }),
            "opaque CURRENT read, not decode");
        check(current.read_exact(112, {}), "empty end read");
        ++positive_cases;
        auto moved = std::move(current);
        check(!current.valid() && !current.recheck() &&
                  current.error().code == RootErrorCode::ClosedOrFailed && moved.valid(),
              "pin move construction");
        PinnedFile assigned;
        assigned = std::move(moved);
        check(!moved.valid() && assigned.recheck(), "pin move assignment");
        auto moved_root = std::move(root);
        check(!root.valid() && !root.pin_current().valid() && moved_root.valid(),
              "root move construction");
        RootLease assigned_root;
        assigned_root = std::move(moved_root);
        check(!moved_root.valid() && assigned_root.recheck(), "root move assignment");
        ++positive_cases;
    }
    io->clean();
    check(f.inventory() == before, "readonly inventory/hash/bytes changed");
    contender(f, true);
    ++positive_cases;
    PinnedFile surviving;
    {
        auto root = RootLease::open_existing(f.path, io);
        surviving = root.pin_current();
        check(surviving.valid(), "dependent acquired");
    }
    contender(f, false);
    check(surviving.recheck(), "dependent lost root lease");
    ++positive_cases;
    surviving = {};
    io->clean();
    std::array<RootRole, 3> last_closes{};
    std::size_t close_count = 0;
    for (std::size_t i = 0; i < io->event_count; ++i) {
        const auto& event = io->events[i];
        if (event.operation == RootOperation::Close && event.stage == RootStage::After)
            last_closes[close_count++ % last_closes.size()] = event.role;
    }
    check(close_count >= 3 && last_closes[(close_count - 3) % 3] == RootRole::Current &&
              last_closes[(close_count - 2) % 3] == RootRole::LockFile &&
              last_closes[(close_count - 1) % 3] == RootRole::Root,
          "dependent/artifact/LOCK/root teardown order");
    contender(f, true);
    // Assignment releases an unrelated prior owner; a pin retains its own root.
    Fixture other;
    other.files();
    {
        auto a = RootLease::open_existing(f.path);
        auto b = RootLease::open_existing(other.path);
        auto old_pin = b.pin_current();
        b = std::move(a);
        contender(other, false);
        old_pin = {};
        contender(other, true);
        check(b.recheck() && !a.valid(), "move assignment ownership");
        auto p = b.pin_current();
        auto q = b.pin_generation_file(ArtifactRole::Tail, gen());
        p = std::move(q);
        check(p.recheck() && !q.valid(), "pin assignment closes old FD");
        ++positive_cases;
    }
    for (const auto& path : {f.path / "", f.path / ".", f.path / "." / ""}) {
        auto root = RootLease::open_existing(path);
        check(root.valid(), "real root terminal slash/dot");
        ++positive_cases;
    }
    check(::mkdir((f.path / "child").c_str(), 0700) == 0, "fixture child");
    {
        auto root = RootLease::open_existing(f.path / "child" / "..");
        check(root.valid(), "preserve trusted .. semantics");
        ++positive_cases;
    }
}

template <class Fn> void root_reject(Fn setup) {
    Fixture f;
    auto path = setup(f);
    const auto before = f.inventory();
    auto io = std::make_shared<Adapter>();
    auto root = RootLease::open_existing(path, io);
    check(!root.valid() && root.error().code != RootErrorCode::None && io->live() == 0,
          "bad root accepted or partial FD retained");
    const auto error = root.error();
    check(!root.recheck() && !root.pin_current().valid() && root.error() == error,
          "root failure not terminal");
    io->clean();
    check(f.inventory() == before, "rejected root modified fixture");
    ++rejection_cases;
}
void unsafe_files() {
    root_reject([](Fixture& f) { return f.path / "missing"; });
    root_reject([](Fixture& f) {
        f.put("plain", "x");
        return f.path / "plain";
    });
    root_reject([](Fixture&) { return std::filesystem::path{}; });
    root_reject([](Fixture& f) {
        return std::filesystem::path{f.path.string() + std::string("\0tail", 5)};
    });
    for (const auto suffix : {"", "/", "/.", "/./"})
        root_reject([&](Fixture& f) {
            check(::symlink(f.path.c_str(), (f.path / "alias").c_str()) == 0,
                  "fixture root symlink");
            return std::filesystem::path{(f.path / "alias").string() + suffix};
        });
    for (int kind = 0; kind < 5; ++kind)
        root_reject([&](Fixture& f) {
            f.remove("LOCK");
            if (kind == 1) {
                f.put("target", "");
                check(::symlink("target", (f.path / "LOCK").c_str()) == 0, "lock symlink");
            }
            if (kind == 2)
                check(::mkdir((f.path / "LOCK").c_str(), 0700) == 0, "lock directory");
            if (kind == 3)
                check(::mkfifo((f.path / "LOCK").c_str(), 0600) == 0, "lock FIFO");
            if (kind == 4) {
                f.put("target", "");
                check(::link((f.path / "target").c_str(), (f.path / "LOCK").c_str()) == 0,
                      "lock hardlink");
            }
            return f.path;
        });
    for (int kind = 0; kind < 5; ++kind) {
        Fixture f;
        if (kind == 1) {
            f.put("target", std::string(112, 'x'));
            check(::symlink("target", (f.path / "CURRENT").c_str()) == 0, "artifact symlink");
        }
        if (kind == 2)
            check(::mkdir((f.path / "CURRENT").c_str(), 0700) == 0, "artifact directory");
        if (kind == 3)
            check(::mkfifo((f.path / "CURRENT").c_str(), 0600) == 0, "artifact FIFO");
        if (kind == 4) {
            f.put("target", std::string(112, 'x'));
            check(::link((f.path / "target").c_str(), (f.path / "CURRENT").c_str()) == 0,
                  "artifact hardlink");
        }
        const auto before = f.inventory();
        auto root = RootLease::open_existing(f.path);
        auto pin = root.pin_current();
        check(!pin.valid() && !root.valid() && !pin.identity(), "unsafe artifact accepted");
        check(f.inventory() == before, "unsafe artifact modified");
        ++rejection_cases;
    }
    for (auto size : {0U, 111U, 113U})
        for (bool marker : {false, true}) {
            Fixture f;
            f.put(marker ? "tail-1.commit" : "CURRENT", std::string(size, 'x'));
            auto root = RootLease::open_existing(f.path);
            auto pin = marker ? root.pin_generation_file(ArtifactRole::Watermark, gen())
                              : root.pin_current();
            check(!pin.valid() && root.error().code == RootErrorCode::InvalidLength,
                  "fixed size rejection");
            ++rejection_cases;
        }
    for (const auto role : {ArtifactRole::Base, ArtifactRole::Tail}) {
        Fixture f;
        f.put(role == ArtifactRole::Base ? "base-1.snap" : "tail-1.aof",
              std::string(role == ArtifactRole::Base ? 203 : 95, 'x'));
        auto root = RootLease::open_existing(f.path);
        auto pin = root.pin_generation_file(role, gen());
        check(!pin.valid() && root.error().code == RootErrorCode::InvalidLength, "minimum size");
        ++rejection_cases;
    }
    for (const auto text :
         {"0", "01", "../1", "1/2", "/1", "1.snap", "18446744073709551616", "-1"}) {
        bool rejected = false;
        try {
            (void)parse_generation(text);
        } catch (const FormatError&) {
            rejected = true;
        }
        check(rejected, "invalid generated name attempt");
        ++rejection_cases;
    }
    {
        Fixture f;
        f.files();
        auto root = RootLease::open_existing(f.path);
        auto p = root.pin_current();
        check(!root.pin_generation_file(static_cast<ArtifactRole>(255), gen()).valid() &&
                  root.error().code == RootErrorCode::InvalidName && !p.valid() && !p.identity(),
              "cross-role request");
        ++rejection_cases;
    }
}

void identity_changes() {
    for (bool lock_file : {false, true})
        for (int change = 0; change < 5; ++change) {
            Fixture f;
            f.files();
            auto io = std::make_shared<Adapter>();
            {
                auto root = RootLease::open_existing(f.path, io);
                auto p = root.pin_current();
                auto peer = root.pin_generation_file(ArtifactRole::Tail, gen());
                const auto name = lock_file ? "LOCK" : "CURRENT";
                if (change <= 1) {
                    check(::rename((f.path / name).c_str(), (f.path / "old").c_str()) == 0,
                          "fixture inode swap");
                    if (change == 0)
                        f.put(name, std::string(lock_file ? 0 : 112, '?'));
                } else if (change == 2)
                    check(::link((f.path / name).c_str(), (f.path / "alias").c_str()) == 0,
                          "identity link count");
                else if (change == 3) {
                    check(::rename((f.path / name).c_str(), (f.path / "old").c_str()) == 0,
                          "fixture symlink swap");
                    check(::symlink("old", (f.path / name).c_str()) == 0, "fixture swapped link");
                } else
                    check(::truncate((f.path / name).c_str(), lock_file ? 1 : 113) == 0,
                          "identity length change");
                const auto after_harness = f.inventory();
                check(!p.recheck() && !root.valid() && !peer.valid() && !p.identity(),
                      "identity change not latched globally");
                const auto error = root.error();
                const auto opens = io->opens;
                check(!root.recheck() && !peer.read_exact(0, {}) && !root.pin_current().valid() &&
                          root.error() == error && io->opens == opens,
                      "failure silently reopened");
                check(f.inventory() == after_harness, "identity rejection modified fixture");
                ++rejection_cases;
            }
            io->clean();
        }
    for (auto edit :
         {Adapter::StatEdit::Device, Adapter::StatEdit::Inode, Adapter::StatEdit::Type}) {
        Fixture f;
        auto io = std::make_shared<Adapter>();
        auto root = RootLease::open_existing(f.path, io);
        io->stat_edit = edit;
        io->edit_role = RootRole::Root;
        check(!root.recheck() && !root.valid(), "held root identity/type recheck");
        ++rejection_cases;
    }
}

void reads_and_bounds() {
    Fixture f;
    std::string binary(200000, '\0');
    for (std::size_t i = 0; i < binary.size(); ++i)
        binary[i] = static_cast<char>(i % 256);
    f.put("tail-1.aof", binary);
    const auto before = f.inventory();
    for (int mode = 0; mode < 3; ++mode) {
        auto io = std::make_shared<Adapter>();
        {
            auto root = RootLease::open_existing(f.path, io);
            auto p = root.pin_generation_file(ArtifactRole::Tail, gen());
            if (mode == 1)
                io->short_limit = 7;
            if (mode == 2)
                io->read_mode = Adapter::ReadMode::InterruptOnce;
            std::vector<std::byte> bytes(binary.size());
            check(p.read_exact(0, bytes) &&
                      std::memcmp(bytes.data(), binary.data(), binary.size()) == 0 &&
                      io->max_request <= 65536,
                  "binary/short/EINTR read");
            check(p.read_exact(123, std::span{bytes}.first(19)), "offset read");
            if (mode == 1)
                check(io->reads > 20000, "short reads not reached");
            ++positive_cases;
        }
        io->clean();
    }
    check(f.inventory() == before, "binary reads mutated files");
    for (const auto offset : {UINT64_MAX, static_cast<std::uint64_t>(INT64_MAX),
                              static_cast<std::uint64_t>(binary.size() + 1),
                              static_cast<std::uint64_t>(binary.size())}) {
        auto root = RootLease::open_existing(f.path);
        auto p = root.pin_generation_file(ArtifactRole::Tail, gen());
        std::array<std::byte, 1> out{};
        check(!p.read_exact(offset, out) && p.error().code == RootErrorCode::InvalidLength &&
                  !p.identity(),
              "read endpoint overflow");
        ++rejection_cases;
    }
    {
        auto root = RootLease::open_existing(f.path);
        auto p = root.pin_generation_file(ArtifactRole::Tail, gen());
        std::array<std::byte, 2> out{};
        check(!p.read_exact(binary.size() - 1, out), "crossing extent");
        ++rejection_cases;
    }
    for (auto mode : {Adapter::ReadMode::Zero, Adapter::ReadMode::Oversized,
                      Adapter::ReadMode::InvalidNegative}) {
        auto io = std::make_shared<Adapter>();
        auto root = RootLease::open_existing(f.path, io);
        auto p = root.pin_generation_file(ArtifactRole::Tail, gen());
        io->read_mode = mode;
        std::array<std::byte, 8> out{};
        check(!p.read_exact(0, out) && !root.valid() && !p.identity(), "invalid read progress");
        ++rejection_cases;
    }
}

void syscall_faults() {
    const std::array<std::pair<RootOperation, RootRole>, 7> acquisition{
        {{RootOperation::Open, RootRole::Root},
         {RootOperation::OpenAt, RootRole::LockFile},
         {RootOperation::Stat, RootRole::Root},
         {RootOperation::Stat, RootRole::LockFile},
         {RootOperation::Lock, RootRole::LockFile},
         {RootOperation::NamedStat, RootRole::LockFile},
         {RootOperation::Stat, RootRole::LockFile}}};
    for (std::size_t point = 0; point < acquisition.size(); ++point) {
        const auto [op, role] = acquisition[point];
        for (auto stage : {RootStage::Before, RootStage::Call, RootStage::After}) {
            Fixture f;
            const auto before = f.inventory();
            auto io = std::make_shared<Adapter>();
            io->arm(op, role, stage);
            // Last point is the second LOCK fstat, AFTER successful flock,
            // rather than duplicating the initial pre-lock metadata failure.
            io->occurrence = point == acquisition.size() - 1 ? 2 : 1;
            auto root = RootLease::open_existing(f.path, io);
            check(!root.valid() && io->hits >= io->occurrence && io->live() == 0,
                  "acquisition fault unreached/leaked");
            const auto e = root.error();
            check(e.operation == op && e.role == role && e.stage == stage && e.system_errno == EIO,
                  "captured acquisition context");
            io->clean();
            check(f.inventory() == before, "failed acquisition mutation");
            ++fault_cases;
        }
    }
    for (auto op : {RootOperation::OpenAt, RootOperation::Stat, RootOperation::NamedStat,
                    RootOperation::Read})
        for (auto stage : {RootStage::Before, RootStage::Call, RootStage::After}) {
            Fixture f;
            f.files();
            const auto before = f.inventory();
            auto io = std::make_shared<Adapter>();
            {
                auto root = RootLease::open_existing(f.path, io);
                PinnedFile pin;
                if (op == RootOperation::Read)
                    pin = root.pin_current();
                io->arm(op, RootRole::Current, stage);
                if (op == RootOperation::Read) {
                    std::array<std::byte, 10> out{};
                    check(!pin.read_exact(0, out), "read fault");
                } else
                    pin = root.pin_current();
                check(!root.valid() && !pin.valid() && io->hits >= 1, "artifact fault unreached");
                const auto e = root.error();
                check(e.operation == op && e.role == RootRole::Current && e.stage == stage &&
                          e.system_errno == EIO,
                      "captured artifact context");
                errno = ENOSPC;
                check(root.error() == e && !pin.recheck() && root.error() == e, "errno not owned");
                ++fault_cases;
            }
            io->clean();
            check(f.inventory() == before, "failed pin/read mutation");
        }
    for (auto [op, role] : {std::pair{RootOperation::Open, RootRole::Root},
                            {RootOperation::Lock, RootRole::LockFile}}) {
        Fixture f;
        auto io = std::make_shared<Adapter>();
        io->arm(op, role, RootStage::Call, op == RootOperation::Open ? EACCES : EWOULDBLOCK);
        auto root = RootLease::open_existing(f.path, io);
        check(!root.valid() &&
                  root.error().code == (op == RootOperation::Open ? RootErrorCode::InvalidRoot
                                                                  : RootErrorCode::LockUnavailable),
              "permissions/contention category");
        io->clean();
        ++fault_cases;
    }
    for (auto edit : {Adapter::StatEdit::Negative, Adapter::StatEdit::Maximum})
        for (auto op : {RootOperation::Stat, RootOperation::NamedStat}) {
            Fixture f;
            f.files();
            auto io = std::make_shared<Adapter>();
            auto root = RootLease::open_existing(f.path, io);
            io->stat_edit = edit;
            io->edit_op = op;
            auto p = root.pin_current();
            check(!p.valid() && root.error().code == RootErrorCode::InvalidLength,
                  "negative/impossible fixed extent");
            ++fault_cases;
        }
    for (auto op : {RootOperation::Open, RootOperation::OpenAt, RootOperation::Stat,
                    RootOperation::NamedStat, RootOperation::Lock}) {
        Fixture f;
        auto io = std::make_shared<Adapter>();
        io->invalid_result = op;
        auto root = RootLease::open_existing(f.path, io);
        check(!root.valid() && root.error().code == RootErrorCode::IoFailure &&
                  root.error().system_errno == EIO,
              "invalid non-read syscall return");
        io->clean();
        ++fault_cases;
    }
    // A successful read cannot hide a later changed observation.
    {
        Fixture f;
        f.files();
        auto io = std::make_shared<Adapter>();
        auto root = RootLease::open_existing(f.path, io);
        auto pin = root.pin_current();
        io->change_after_read = true;
        std::array<std::byte, 1> bytes{};
        check(!pin.read_exact(0, bytes) && !root.valid(), "post-read identity check missing");
        ++fault_cases;
    }
    // Real inode truncation between prechecks and pread is a harness-only action.
    {
        Fixture f;
        f.files();
        auto io = std::make_shared<Adapter>();
        auto root = RootLease::open_existing(f.path, io);
        auto pin = root.pin_current();
        const auto path = f.path / "CURRENT";
        io->truncate_on_read = path.c_str();
        std::array<std::byte, 1> bytes{};
        check(!pin.read_exact(0, bytes) && pin.error().code == RootErrorCode::Truncated,
              "actual pread EOF trusted old stat size");
        check(std::filesystem::file_size(path) == 0, "primitive repaired truncated fixture");
        ++fault_cases;
    }
}

void close_and_allocation_faults() {
    for (auto role : {RootRole::Current, RootRole::LockFile, RootRole::Root})
        for (auto stage : {RootStage::Before, RootStage::Call, RootStage::After})
            for (auto err : {EIO, EINTR}) {
                Fixture f;
                f.files();
                const auto before = f.inventory();
                auto io = std::make_shared<Adapter>();
                {
                    auto root = RootLease::open_existing(f.path, io);
                    if (role == RootRole::Current) {
                        auto p = root.pin_current();
                        auto peer = root.pin_generation_file(ArtifactRole::Tail, gen());
                        io->arm(RootOperation::Close, role, stage, err);
                        const auto closes = io->closes;
                        check(!p.close_checked() && !p.valid() && !peer.valid() &&
                                  io->closes == closes + 1,
                              "failed checked file close");
                        check(!p.close_checked() && io->closes == closes + 1,
                              "close retried relinquished FD");
                    } else {
                        io->arm(RootOperation::Close, role, stage, err);
                        check(!root.close_checked(), "failed checked root close");
                    }
                    check(!root.valid() && root.error().operation == RootOperation::Close &&
                              root.error().stage == stage && root.error().system_errno == err &&
                              io->hits == 1,
                          "close fault context/retry");
                    ++fault_cases;
                }
                io->clean();
                check(f.inventory() == before, "failed close mutation");
            }
    for (bool destruction : {false, true}) {
        Fixture f;
        f.files();
        auto io = std::make_shared<Adapter>();
        {
            auto root = RootLease::open_existing(f.path, io);
            const auto path = f.path / "CURRENT";
            {
                auto pin = root.pin_current();
                io->arm(RootOperation::Close, RootRole::Current, RootStage::Call, EINTR);
                io->reuse_path = path.c_str();
                if (!destruction)
                    check(!pin.close_checked(), "ambiguous checked-close fixture");
            }
            check(!root.valid() && root.error().system_errno == EINTR && io->hits == 1 &&
                      io->reused_fd >= 0 && ::fcntl(io->reused_fd, F_GETFD) >= 0,
                  "destructor/checked close retried reused number");
            ++fault_cases;
        }
        check(::fcntl(io->reused_fd, F_GETFD) >= 0, "later teardown closed reused descriptor");
        check(::close(io->reused_fd) == 0, "harness reuse FD close");
        io->clean();
    }
    {
        Fixture f;
        auto io = std::make_shared<Adapter>();
        auto root = RootLease::open_existing(f.path, io);
        io->invalid_result = RootOperation::Close;
        check(!root.close_checked() && root.error().system_errno == EIO, "invalid close return");
        io->clean();
        ++fault_cases;
    }
    {
        Fixture f;
        f.files();
        auto root = RootLease::open_existing(f.path);
        auto p = root.pin_current();
        check(!root.close_checked() && root.error().code == RootErrorCode::DependentsAlive &&
                  !p.valid(),
              "premature root close");
        contender(f, false);
        ++rejection_cases;
    }
    {
        Fixture f;
        auto io = std::make_shared<Adapter>();
        auto root = RootLease::open_existing(f.path, io);
        check(root.close_checked() && !root.valid() && !root.close_checked(),
              "successful checked root close");
        io->clean();
        ++positive_cases;
    }
    Fixture f;
    f.files();
    const auto before = f.inventory();
    auto io = std::make_shared<Adapter>();
    bool completed = false;
    // Existing root with many terminal dots forces path bookkeeping allocations,
    // without creating any additional directory or following a symlink.
    auto path = f.path;
    for (int i = 0; i < 8; ++i)
        path /= ".";
    for (long budget = 0; budget < 200; ++budget) {
        allocation_budget = budget;
        auto root = RootLease::open_existing(path, io);
        const bool valid = root.valid();
        const auto error = root.error();
        allocation_budget = -1;
        if (valid) {
            completed = true;
            check(root.close_checked(), "allocation campaign close");
            break;
        }
        check(error.code == RootErrorCode::ResourceLimit && io->live() == 0,
              "allocation failure ownership");
        ++allocation_failures;
    }
    check(completed && allocation_failures > 2, "allocation campaign missing");
    for (long budget : {0L, 1L}) {
        allocation_budget = budget;
        auto root = RootLease::open_existing(f.path);
        allocation_budget = -1;
        check(!root.valid() && root.error().code == RootErrorCode::ResourceLimit,
              "default adapter allocation failure");
        ++allocation_failures;
    }
    {
        auto root = RootLease::open_existing(f.path, io);
        allocation_budget = 0;
        auto p = root.pin_generation_file(ArtifactRole::Watermark, gen());
        std::array<std::byte, 112> bytes{};
        const bool ok = p.valid() && p.read_exact(0, bytes);
        allocation_budget = -1;
        check(ok, "pin/recheck/read unexpectedly allocate");
        ++positive_cases;
    }
    io->clean();
    check(f.inventory() == before, "allocation failures mutated fixture");
}
std::array<bool, 512> open_descriptors() {
    std::array<bool, 512> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = ::fcntl(static_cast<int>(i), F_GETFD) >= 0;
    return result;
}
} // namespace

int main() {
    try {
        const auto descriptors = open_descriptors();
        basics_and_lifetimes();
        unsafe_files();
        identity_changes();
        reads_and_bounds();
        syscall_faults();
        close_and_allocation_faults();
        check(open_descriptors() == descriptors, "process descriptor inventory changed");
        std::cout
            << "PASS V2 root: " << positive_cases << " positive cases, " << rejection_cases
            << " rejection cases, " << fault_cases << " syscall/boundary cases, "
            << allocation_failures
            << " allocation failures; owned fixtures only; no production writes/publication\n";
    } catch (const std::exception& e) {
        allocation_budget = -1;
        std::cerr << e.what() << '\n';
        return 1;
    }
}
