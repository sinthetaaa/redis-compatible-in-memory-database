#include "redisdb/persistence/V2PendingBaseWrite.hpp"
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

#include <bit>
#include <chrono>
namespace {
struct alignas(std::max_align_t) Allocation {
    std::size_t size;
};
struct AllocationPoint {
    std::size_t size, operation;
};
std::array<AllocationPoint, 8192> allocation_trace{};
std::size_t live_allocations = 0, live_bytes = 0;
std::size_t allocation_calls = 0, armed = 0, reached = 0, operation = 0;
std::size_t largest_request = 0, peak_live = 0;
bool observe_allocations = false;
} // namespace
void* operator new(std::size_t n) {
    if (observe_allocations) {
        if (allocation_calls == allocation_trace.size())
            std::abort();
        allocation_trace[allocation_calls++] = {n, operation};
        largest_request = std::max(largest_request, n);
        if (armed && allocation_calls == armed) {
            ++reached;
            throw std::bad_alloc{};
        }
    }
    if (n > SIZE_MAX - sizeof(Allocation))
        throw std::bad_alloc{};
    auto* block = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + (n ? n : 1)));
    if (!block)
        throw std::bad_alloc{};
    block->size = n;
    ++live_allocations;
    live_bytes += n;
    if (observe_allocations)
        peak_live = std::max(peak_live, live_bytes);
    return block + 1;
}
[[gnu::noinline]] void operator delete(void* p) noexcept {
    if (p) {
        auto* block = static_cast<Allocation*>(p) - 1;
        live_bytes -= block->size;
        --live_allocations;
        std::free(block);
    }
}
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
[[gnu::noinline]] void operator delete[](void* p) noexcept { ::operator delete(p); }
[[gnu::noinline]] void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace redisdb;
using namespace persistence::v2;
void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
GenerationId gen(std::uint64_t n = 1) { return GenerationId{n}; }
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

bool final_role(RootRole r) { return r >= RootRole::Base && r <= RootRole::Watermark; }
bool staged_role(RootRole r) { return r >= RootRole::StagedBase && r <= RootRole::StagedWatermark; }
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
    std::uint64_t accepted_bytes = 0;
    std::size_t max_write = 0, short_limit = SIZE_MAX, write_fail_at = 0, write_interrupts = 0;
    int write_answer = -1, write_errno = EIO;
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
        ++writes;
        max_write = std::max(max_write, n);
        if (forbid_content || active != RootRole::StagedBase || n > 65536)
            std::abort();
        if (!call(RootOperation::WriteAt, active, fd))
            return -1;
        if (write_interrupts) {
            --write_interrupts;
            errno = EINTR;
            return -1;
        }
        if (write_fail_at == writes) {
            errno = write_errno;
            return write_answer == 999 ? static_cast<ssize_t>(n + 1) : write_answer;
        }
        const auto result = RootIo::write_at(fd, p, std::min(n, short_limit), off);
        if (result > 0)
            accepted_bytes += static_cast<std::uint64_t>(result);
        return result;
    }
    int sync_file(int) noexcept override { std::abort(); }
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
std::uint32_t reverse(std::uint32_t n) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 32; ++i) {
        result = (result << 1) | (n & 1U);
        n >>= 1;
    }
    return result;
}
// Independent forward Castagnoli polynomial, reflected input/output.
std::uint32_t crc(std::string_view data, std::uint32_t prior = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto r = i << 24;
            for (unsigned j = 0; j < 8; ++j)
                r = (r << 1) ^ ((r & 0x80000000U) ? 0x1edc6f41U : 0U);
            result[i] = r;
        }
        return result;
    }();
    static const auto reflected = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i)
            result[i] = reverse(i) >> 24;
        return result;
    }();
    auto r = reverse(~prior);
    for (const char c : data)
        r = (r << 8) ^ table[(r >> 24) ^ reflected[static_cast<unsigned char>(c)]];
    return ~reverse(r);
}
void put(std::string& target, std::size_t at, std::uint64_t n, unsigned width) {
    check(at <= target.size() && width <= target.size() - at, "oracle BE bounds");
    for (unsigned i = width; i != 0; --i)
        target[at + width - i] = static_cast<char>((n >> ((i - 1) * 8)) & 255U);
}
std::string be(std::uint64_t n, unsigned width) {
    std::string result(width, '\0');
    put(result, 0, n, width);
    return result;
}
std::uint32_t get32(std::string_view data, std::size_t at) {
    check(at <= data.size() && 4 <= data.size() - at, "oracle u32 bounds");
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i)
        n = (n << 8) | static_cast<unsigned char>(data[at + i]);
    return n;
}
std::string seal(std::string prefix) { return prefix + be(crc(prefix), 4) + "END!"; }
std::string frame(std::string_view tag, std::string_view payload) {
    return seal(std::string{tag} + be(payload.size(), 4) + std::string{payload});
}
std::string blob(std::string_view s) { return be(s.size(), 4) + std::string{s}; }
std::int64_t micros(const storage::Entry& e) {
    return std::chrono::floor<std::chrono::microseconds>(e.expires_at->time_since_epoch()).count();
}
std::string value_bytes(const storage::Entry& e) {
    std::string result;
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, storage::StringValue>) {
                result = blob(value.bytes);
            } else if constexpr (std::is_same_v<T, storage::ListValue>) {
                result = be(value.elements.size(), 4);
                for (const auto& item : value.elements)
                    result += blob(item);
            } else if constexpr (std::is_same_v<T, storage::HashValue>) {
                std::vector<std::string> keys;
                for (const auto& [key, field_value] : value.fields) {
                    (void)field_value;
                    keys.push_back(key);
                }
                std::sort(keys.begin(), keys.end());
                result = be(keys.size(), 4);
                for (const auto& key : keys)
                    result += blob(key) + blob(value.fields.at(key));
            } else {
                std::vector<std::string> members(value.members.begin(), value.members.end());
                std::sort(members.begin(), members.end());
                result = be(members.size(), 4);
                for (const auto& member : members)
                    result += blob(member);
            }
        },
        e.value);
    return result;
}
std::string key_frame(std::string_view key, const storage::Entry& e) {
    auto payload =
        be(1, 4) + blob(key) + be(e.value.index() + 1, 1) + be(e.expires_at.has_value(), 1);
    if (e.expires_at)
        payload += be(std::bit_cast<std::uint64_t>(micros(e)), 8);
    payload += value_bytes(e);
    payload += std::string(5, '\0');
    return frame("KEY1", payload);
}
std::size_t charge(std::string_view key, const storage::Entry& e) {
    std::size_t result = 64 + key.size() + (e.expires_at ? 48 + key.size() : 0);
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, storage::StringValue>) {
                result += value.bytes.size();
            } else if constexpr (std::is_same_v<T, storage::ListValue>) {
                for (const auto& item : value.elements)
                    result += 48 + item.size();
            } else if constexpr (std::is_same_v<T, storage::HashValue>) {
                for (const auto& [field, item] : value.fields)
                    result += 80 + field.size() + item.size();
            } else {
                for (const auto& member : value.members)
                    result += 56 + member.size();
            }
        },
        e.value);
    return result;
}
BaseEncodingContext context() {
    StoreBytes store{};
    for (std::size_t i = 0; i < store.size(); ++i)
        store[i] = static_cast<std::byte>(i);
    return {StoreId{store},
            GenerationId{std::uint64_t{1}},
            GenerationId{std::uint64_t{2}},
            LogicalSequence{std::uint64_t{1}},
            {1, 144, 0xad924c74U, 0x04430f74U}};
}
BaseEncodeLimits limits() { return {SIZE_MAX, MaxFileBytes, MaxKeyPayload, SIZE_MAX, SIZE_MAX}; }
struct Data {
    BaseEncodingContext ctx = context();
    std::vector<std::string> keys;
    std::vector<storage::Entry> entries;
    std::vector<BaseEntryView> views;
    std::vector<std::size_t> order;
    void add(std::string key, storage::Value value,
             std::optional<storage::WallTimePoint> deadline = {}) {
        keys.push_back(std::move(key));
        entries.push_back(storage::Entry{std::move(value), deadline});
        entries.back().accounted_charge = 999; // Deliberately ignored cached charge.
    }
    void bind() {
        views.clear();
        order.clear();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            views.push_back({keys[i], &entries[i]});
            order.push_back(entries.size() - 1 - i);
        }
    }
    BaseInputView input(bool lru) const {
        return {views, lru,
                lru ? std::span<const std::size_t>{order} : std::span<const std::size_t>{}};
    }
    std::string fingerprint() const {
        std::string result;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            result += blob(keys[i]) + value_bytes(entries[i]);
            result += be(entries[i].accounted_charge, 8);
            result += be(entries[i].expires_at.has_value(), 1);
            if (entries[i].expires_at)
                result += be(
                    std::bit_cast<std::uint64_t>(entries[i].expires_at->time_since_epoch().count()),
                    8);
        }
        for (const auto i : order)
            result += be(i, 8);
        return result;
    }
};
std::string header(const Data& f, bool lru) {
    std::string b(176, '\0');
    b.replace(0, 8, "RDBBAS02");
    put(b, 8, 2, 4);
    put(b, 12, lru, 4);
    put(b, 16, 184, 4);
    put(b, 20, 1, 4);
    for (std::size_t i = 0; i < 16; ++i) {
        b[24 + i] = std::bit_cast<char>(std::to_integer<unsigned char>(f.ctx.store.bytes()[i]));
        b[88 + i] = b[24 + i];
    }
    put(b, 40, f.ctx.candidate_generation.value(), 8);
    put(b, 48, f.ctx.captured_sequence.value(), 8);
    put(b, 56, f.views.size(), 8);
    std::size_t q = 0;
    for (const auto& view : f.views)
        q += charge(view.key, *view.entry);
    put(b, 64, q, 8);
    put(b, 72, lru ? f.views.size() : 0, 8);
    put(b, 80, 2, 4);
    put(b, 104, f.ctx.selected_generation.value(), 8);
    for (const auto at : {112U, 136U}) {
        put(b, at, f.ctx.synchronized_source.sequence, 8);
        put(b, at + 8, f.ctx.synchronized_source.offset, 8);
        put(b, at + 16, f.ctx.synchronized_source.record_crc, 4);
        put(b, at + 20, f.ctx.synchronized_source.prefix_crc, 4);
    }
    return seal(b);
}
std::string reference(const Data& f, bool lru) {
    auto b = header(f, lru);
    for (const auto& view : f.views)
        b += key_frame(view.key, *view.entry);
    if (lru)
        for (const auto i : f.order)
            b += frame("LRU1", f.views[i].key);
    return b + frame("DONE", be(crc(b), 4));
}
static_assert(std::is_final_v<PendingBaseWriter>);
static_assert(!std::is_copy_constructible_v<PendingBaseWriter>);
static_assert(!std::is_copy_assignable_v<PendingBaseWriter>);
static_assert(std::is_nothrow_move_constructible_v<PendingBaseWriter>);
static_assert(std::is_nothrow_move_assignable_v<PendingBaseWriter>);
static_assert(std::is_nothrow_destructible_v<PendingBaseWriter>);
static_assert(noexcept(PendingBaseWriter::begin(std::declval<PendingGeneration&&>(),
                                                std::declval<BaseEncodingContext>(),
                                                std::declval<BaseInputView>(),
                                                std::declval<BaseEncodeLimits>())));
static_assert(noexcept(std::declval<PendingBaseWriter&>().advance()));
template <class T>
concept Escape = requires(T& t) { t.append(ByteView{}); } || requires(T& t) { t.seal_file(); } ||
                 requires(T& t) { t.fd(); } || requires(T& t) { t.filename(); } ||
                 requires(T& t) { t.staged(ArtifactRole::Base); } ||
                 requires(T& t) { t.publish(); } || requires(T& t) { t.root(); };
static_assert(!Escape<PendingBaseWriter>);
std::size_t scenarios = 0, planned = 0, injected = 0, validated = 0;
std::size_t allocation_planned = 0, allocation_reached = 0, allocation_validated = 0;
std::size_t move_cases = 0, move_planned = 0, move_reached = 0, move_validated = 0;
template <class Fn> void scenario(const char* name, Fn fn) {
    fn();
    ++scenarios;
    std::cout << "SCENARIO " << scenarios << " PASS " << name << '\n';
}
struct Environment {
    Fixture disk;
    std::shared_ptr<Adapter> io = std::make_shared<Adapter>();
    RootLease root = RootLease::open_existing(disk.path, io);
    Environment() {
        check(root.valid(), "open root");
        disk.files();
    }
    ~Environment() noexcept {
        root = RootLease{};
        if (io->live() || !io->tracking_ok || !io->flags_ok || io->syncs || io->reads)
            std::abort();
    }
    PendingGeneration pending() {
        auto p = PendingGeneration::begin_after(root, gen(), LogicalSequence{std::uint64_t{1}}, 8);
        check(p.valid(), "empty aggregate");
        io->forbid_content = false;
        io->forbid_reopen = true;
        return p;
    }
    PendingBaseWriter writer(const Data& d, bool lru = false) {
        auto p = pending();
        const auto count = io->used, opens = io->opens, creates = io->creates;
        auto w = PendingBaseWriter::begin(std::move(p), d.ctx, d.input(lru), limits());
        check(w.error().code == PendingBaseWriteError::None && !p.valid(), "transfer");
        check(count == io->used && opens == io->opens && creates == io->creates,
              "transfer performed I/O");
        check(io->live() == 5 && io->temp_count == 3, "same three descriptors");
        return w;
    }
    std::filesystem::path base() const { return disk.path / io->temps[0].name.data(); }
    std::string stored() const {
        const auto fd = ::open(base().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        check(fd >= 0, "oracle read open");
        std::string result;
        std::array<char, 65536> b{};
        for (;;) {
            const auto n = ::read(fd, b.data(), b.size());
            check(n >= 0, "oracle read");
            if (!n)
                break;
            result.append(b.data(), static_cast<std::size_t>(n));
        }
        check(::close(fd) == 0, "oracle read close");
        return result;
    }
    void retained_extent() const {
        struct stat s{};
        check(::lstat(base().c_str(), &s) == 0 && s.st_size >= 0 &&
                  static_cast<std::uint64_t>(s.st_size) == io->accepted_bytes,
              "retained real extent");
        for (std::size_t i = 1; i < 3; ++i)
            check(::lstat((disk.path / io->temps[i].name.data()).c_str(), &s) == 0 &&
                      s.st_size == 0,
                  "sibling content changed");
    }
};
void complete(PendingBaseWriter& w) {
    while (w.advance() == PendingBaseWriteStep::More) {
    }
    check(w.written_unsealed() && w.generated() && w.error().code == PendingBaseWriteError::None,
          "WrittenUnsealed completion");
}
void terminal(PendingBaseWriter& w) {
    const auto e = w.error();
    check(e.code != PendingBaseWriteError::None && e.code != PendingBaseWriteError::Closed,
          "missing terminal error");
    for (int i = 0; i < 3; ++i) {
        check(w.advance() == PendingBaseWriteStep::Error && !w.generated() &&
                  !w.written_unsealed() && !w.recheck() && !w.discard_checked() &&
                  !w.close_checked(),
              "terminal recovery");
        const auto next = w.error();
        check(next.code == e.code && next.root == e.root && next.encoding.code == e.encoding.code,
              "first error changed");
    }
}
void selected_unchanged(const Fixture::Inventory& old, const Fixture::Inventory& now) {
    for (const auto& [name, item] : old) {
        const auto it = now.find(name);
        check(it != now.end() && it->second == item, "protected file changed");
    }
}
Data sample() {
    Data d;
    d.add("", storage::StringValue{""});
    d.add(std::string("a\0", 2), storage::StringValue{"00012"},
          storage::WallTimePoint{storage::WallTimePoint::duration{-1}});
    d.add("b", storage::ListValue{{"x", "x", ""}});
    storage::HashValue h;
    h.fields.emplace("z", "1");
    h.fields.emplace("a", "2");
    d.add("c", std::move(h));
    storage::SetValue s;
    s.members.emplace("z");
    s.members.emplace("a");
    d.add(std::string(1, static_cast<char>(0x80)), std::move(s),
          storage::WallTimePoint{std::chrono::seconds{2000000000}});
    d.bind();
    return d;
}
void success_cases() {
    for (int kind = 0; kind < 4; ++kind)
        for (bool lru : {false, true})
            scenario("wire/CRC/extent and ownership", [=] {
                Data d;
                if (kind == 1)
                    d.add("key", storage::StringValue{"value"});
                if (kind == 2)
                    d = sample();
                if (kind == 3)
                    d.add(std::string(MaxLruPayload, 'k'), storage::StringValue{""});
                d.bind();
                const auto before = d.fingerprint();
                Environment e;
                const auto selected = e.disk.inventory();
                auto w = e.writer(d, lru);
                auto busy = PendingGeneration::begin_after(e.root, gen(),
                                                           LogicalSequence{std::uint64_t{1}}, 8);
                check(!busy.valid() && busy.error().code == RootErrorCode::WrongState &&
                          e.root.valid(),
                      "admission released during transfer");
                e.io->capture = false;
                complete(w);
                check(w.recheck(), "recheck written");
                const auto expected = reference(d, lru), actual = e.stored();
                check(actual == expected &&
                          w.generated()->identity().byte_length == actual.size() &&
                          w.generated()->identity().file_crc.value() == crc(actual) &&
                          w.generated()->identity().header_crc.value() == get32(actual, 176),
                      "independent wire/CRC");
                BaseStreamValidator oracle{w.generated()->identity()};
                const auto view = std::as_bytes(std::span{actual.data(), actual.size()});
                check(oracle.feed(view) == StreamStatus::NeedMore &&
                          oracle.finish() == StreamStatus::Complete,
                      "independent serialized validation");
                check(oracle.result()->entries().size() == d.entries.size(),
                      "decoded physical count");
                check(d.fingerprint() == before, "borrowed physical input changed");
                selected_unchanged(selected, e.disk.inventory());
                e.retained_extent();
                const auto writes = e.io->writes;
                check(w.advance() == PendingBaseWriteStep::WrittenUnsealed &&
                          e.io->writes == writes,
                      "completion rewrote data");
                check(w.discard_checked() && !w.generated() && e.io->live() == 2,
                      "healthy discard");
                e.io->forbid_reopen = false;
                auto next = e.pending();
                check(next.valid(), "claim not released");
            });
    scenario("raw APIs still available and same RootState after root move", [] {
        Data d;
        d.bind();
        Environment e;
        auto w = e.writer(d);
        auto moved = std::move(e.root);
        auto raw = StagedFile::create_data(moved, ArtifactRole::Base, gen(4));
        auto candidate =
            GenerationCandidate::find_after(moved, gen(), LogicalSequence{std::uint64_t{1}}, 8);
        check(raw.valid() && candidate.valid() && *candidate.generation() == gen(2),
              "raw APIs changed");
        complete(w);
        check(w.close_checked(), "retain close");
        e.retained_extent();
        check(raw.discard_checked(), "raw discard");
        e.root = std::move(moved);
    });
}
void preflight_cases() {
    scenario("empty-input begin and transfer allocate nothing", [] {
        Data d;
        d.bind();
        Environment e;
        auto p = e.pending();
        const auto calls = e.io->used;
        allocation_calls = reached = 0;
        armed = 1;
        observe_allocations = true;
        auto w = PendingBaseWriter::begin(std::move(p), d.ctx, d.input(false), limits());
        observe_allocations = false;
        armed = 0;
        check(w.error().code == PendingBaseWriteError::None && allocation_calls == 0 &&
                  reached == 0 && e.io->used == calls && !p.valid(),
              "ownership transfer allocated or performed I/O");
        check(w.discard_checked(), "allocation-free owner disposal");
    });
    scenario("default and empty source", [] {
        PendingBaseWriter w;
        check(w.error().code == PendingBaseWriteError::Closed &&
                  w.advance() == PendingBaseWriteStep::Error && !w.generated(),
              "default owner");
        Data d;
        d.bind();
        PendingGeneration p;
        auto bad = PendingBaseWriter::begin(std::move(p), d.ctx, d.input(false), limits());
        check(bad.error().code == PendingBaseWriteError::WrongState, "empty source accepted");
    });
    for (int kind = 0; kind < 13; ++kind)
        scenario("pre-transfer source preservation", [=] {
            auto d = sample();
            Environment e;
            auto p = e.pending();
            auto ctx = d.ctx;
            auto lim = limits();
            auto input = d.input(true);
            switch (kind) {
            case 0:
                ctx.selected_generation = gen(3);
                break;
            case 1:
                ctx.candidate_generation = gen(4);
                break;
            case 2:
                ctx.captured_sequence = LogicalSequence{std::uint64_t{2}};
                break;
            case 3:
                ctx.synchronized_source.sequence = 0;
                break;
            case 4:
                lim.max_bytes = 203;
                break;
            case 5:
                lim.max_entries = 0;
                break;
            case 6:
                lim.max_frame_payload = 4;
                break;
            case 7:
                lim.max_permutation_bytes = 0;
                break;
            case 8:
                lim.max_logical_charge = 0;
                break;
            case 9:
                d.views[0].entry = nullptr;
                break;
            case 10:
                std::swap(d.views[0], d.views[1]);
                break;
            case 12:
                d.views[1].key = d.views[0].key;
                break;
            default:
                d.order[0] = d.order[1];
                break;
            }
            const auto before = e.disk.inventory();
            const auto count = e.io->used;
            auto w = PendingBaseWriter::begin(std::move(p), ctx, input, lim);
            check(w.error().code == (kind < 3 ? PendingBaseWriteError::ContextMismatch
                                              : PendingBaseWriteError::Encoding),
                  "wrong local rejection");
            terminal(w);
            check(p.valid() && e.root.valid() && e.io->used == count &&
                      e.disk.inventory() == before,
                  "pure rejection consumed or poisoned source");
            check(p.discard_checked(), "preserved source disposal");
        });
    scenario("existing root error wins", [] {
        Data d;
        d.bind();
        Environment e;
        auto p = e.pending();
        e.io->edit_role = RootRole::Root;
        check(!e.root.recheck(), "root injection");
        const auto first = e.root.error();
        const auto count = e.io->used;
        auto w = PendingBaseWriter::begin(std::move(p), d.ctx, d.input(false), limits());
        check(w.error().root == first && w.error().code == PendingBaseWriteError::RootFailure &&
                  e.io->used == count,
              "root error precedence");
        terminal(w);
    });
}
// One fresh isolated owner per healthy-trace ordinal; no synthetic successful writes.
void io_campaign() {
    for (int action = 0; action < 6; ++action)
        for (bool last : {false, true}) {
            std::vector<RootIoEvent> trace;
            {
                Data d;
                d.bind();
                Environment e;
                auto w = e.writer(d);
                if (action >= 2)
                    complete(w);
                if (last)
                    e.root = RootLease{};
                e.io->reset();
                if (action == 0)
                    complete(w);
                else if (action == 1 || action == 3)
                    check(w.discard_checked(), "discover discard");
                else if (action == 2)
                    check(w.recheck(), "discover recheck");
                else if (action == 4)
                    check(w.close_checked(), "discover retain");
                else {
                    PendingBaseWriter empty;
                    w = std::move(empty);
                }
                check(e.io->used <= e.io->trace.size(), "trace capacity");
                for (std::size_t n = 0; n < e.io->used; ++n)
                    trace.push_back(e.io->trace[n].event);
                e.io->capture = false;
            }
            for (std::size_t i = 0; i < trace.size(); ++i)
                std::cout << "IO_TRACE action=" << action << " last=" << last
                          << " ordinal=" << i + 1 << " op=" << static_cast<int>(trace[i].operation)
                          << " role=" << static_cast<int>(trace[i].role)
                          << " stage=" << static_cast<int>(trace[i].stage) << '\n';
            planned += trace.size();
            std::cout << "IO_DISCOVERY action=" << action << " last=" << last
                      << " eligible=" << trace.size() << '\n';
            for (std::size_t ordinal = 1; ordinal <= trace.size(); ++ordinal) {
                Data d;
                d.bind();
                Environment e;
                auto w = e.writer(d);
                if (action >= 2)
                    complete(w);
                if (last)
                    e.root = RootLease{};
                e.io->reset();
                e.io->target = ordinal;
                if (action == 0) {
                    while (w.advance() == PendingBaseWriteStep::More) {
                    }
                } else if (action == 1 || action == 3)
                    (void)w.discard_checked();
                else if (action == 2)
                    (void)w.recheck();
                else if (action == 4)
                    (void)w.close_checked();
                else {
                    PendingBaseWriter empty;
                    w = std::move(empty);
                }
                check(e.io->reached == 1, "unreached planned I/O injection");
                ++injected;
                const auto expected = trace[ordinal - 1];
                const auto observed = e.io->trace[ordinal - 1].event;
                check(observed.operation == expected.operation && observed.role == expected.role &&
                          observed.stage == expected.stage,
                      "healthy/fault trace divergence");
                const auto err = w.error().root;
                check(err.code == RootErrorCode::IoFailure && err.operation == expected.operation &&
                          err.role == expected.role && err.stage == expected.stage &&
                          err.system_errno == EIO,
                      "wrong exact I/O error mapping");
                terminal(w);
                check(e.io->live() == (last ? 0U : 2U) && e.io->unlinks_after_fault == 0,
                      "failed owner leaked or unlinked after failure");
                if (action == 0)
                    e.retained_extent();
                ++validated;
                ++scenarios;
                std::cout << "IO_INJECTION action=" << action << " last=" << last
                          << " ordinal=" << ordinal
                          << " reached=1 validated=1 op=" << static_cast<int>(err.operation)
                          << " role=" << static_cast<int>(err.role)
                          << " stage=" << static_cast<int>(err.stage)
                          << " actual_bytes=" << e.io->accepted_bytes << '\n';
                e.io->capture = false;
            }
        }
}
void write_cases() {
    for (int kind = 0; kind < 4; ++kind)
        scenario("actual owned-name replacement is never followed", [=] {
            Data d;
            d.bind();
            Environment e;
            auto w = e.writer(d);
            const auto base = e.base(), saved = e.disk.path / "owned-original";
            check(::rename(base.c_str(), saved.c_str()) == 0, "save owned inode");
            if (kind == 0)
                check(::symlink("owned-original", base.c_str()) == 0, "replace symlink");
            else if (kind == 1)
                check(::link(saved.c_str(), base.c_str()) == 0, "replace hardlink");
            else if (kind == 2)
                check(::mkdir(base.c_str(), 0700) == 0, "replace directory");
            else
                check(::mkfifo(base.c_str(), 0600) == 0, "replace FIFO");
            const auto before = e.disk.inventory();
            check(w.advance() == PendingBaseWriteStep::Error && e.io->writes == 0,
                  "replaced name accepted");
            terminal(w);
            check(e.disk.inventory() == before && e.io->unlinks == 0,
                  "untrusted replacement changed or unlinked");
        });
    scenario("real short writes and EINTR", [] {
        auto d = sample();
        Environment e;
        auto w = e.writer(d, true);
        e.io->short_limit = 13;
        e.io->write_interrupts = 2;
        e.io->capture = false;
        complete(w);
        check(e.stored() == reference(d, true) && e.io->max_write <= 65536, "short-write loop");
        check(w.discard_checked(), "short discard");
    });
    for (int code : {ENOSPC, EIO, EFBIG, EINTR})
        for (int at : {1, 2, 3})
            scenario("actual syscall failure and partial progress", [=] {
                auto d = sample();
                Environment e;
                auto w = e.writer(d, true);
                e.io->short_limit = 13;
                e.io->write_fail_at = static_cast<std::size_t>(at);
                e.io->write_errno = code;
                e.io->capture = false;
                if (code == EINTR) {
                    complete(w);
                    check(e.stored() == reference(d, true), "EINTR retry");
                } else {
                    check(w.advance() == PendingBaseWriteStep::Error, "failed write accepted");
                    const auto err = w.error().root;
                    check(err.operation == RootOperation::WriteAt &&
                              err.role == RootRole::StagedBase && err.stage == RootStage::Call &&
                              err.system_errno == code,
                          "write errno changed");
                    terminal(w);
                    e.retained_extent();
                    check(e.io->accepted_bytes == 13U * static_cast<unsigned>(at - 1),
                          "partial progress lost/replayed");
                }
            });
    for (int result : {0, -2, 999})
        scenario("invalid syscall progress", [=] {
            Data d;
            d.bind();
            Environment e;
            auto w = e.writer(d);
            e.io->write_fail_at = 1;
            e.io->write_answer = result;
            check(w.advance() == PendingBaseWriteStep::Error, "invalid write accepted");
            terminal(w);
            e.retained_extent();
            check(w.error().root.system_errno == EIO, "invalid progress diagnostic");
        });
    for (bool named : {false, true})
        for (auto role : {RootRole::Root, RootRole::LockFile, RootRole::StagedBase,
                          RootRole::StagedTail, RootRole::StagedWatermark})
            for (auto edit : {Adapter::Edit::Inode, Adapter::Edit::Device, Adapter::Edit::Type,
                              Adapter::Edit::Links}) {
                if (role == RootRole::Root && (named || edit == Adapter::Edit::Links))
                    continue;
                scenario("identity/type/link mutation", [=] {
                    Data d;
                    d.bind();
                    Environment e;
                    auto w = e.writer(d);
                    e.io->edit_role = role;
                    e.io->edit_named = named;
                    e.io->edit = edit;
                    check(w.advance() == PendingBaseWriteStep::Error, "mutation accepted");
                    terminal(w);
                    check(e.io->writes == 0, "write after identity failure");
                });
            }
}
void moves() {
    for (int phase = 0; phase < 3; ++phase)
        scenario("move/self-move/retain lifecycle", [=] {
            auto d = sample();
            Environment e;
            auto w = e.writer(d, true);
            e.io->capture = false; // Counts remain live; this is not a discovery trace.
            if (phase > 0)
                check(w.advance() == PendingBaseWriteStep::More, "header write");
            if (phase == 2)
                complete(w);
            const auto count = e.io->used;
            auto moved = std::move(w);
            PendingBaseWriter target;
            target = std::move(moved);
            auto* self = &target;
            target = std::move(*self);
            check(e.io->used == count && w.error().code == PendingBaseWriteError::Closed &&
                      moved.error().code == PendingBaseWriteError::Closed,
                  "move I/O or source ownership");
            complete(target);
            check(e.stored() == reference(d, true), "move buffer lifetime");
            check(target.close_checked() && !target.generated(), "retain close");
            e.retained_extent();
            ++move_cases;
        });
    scenario("root close with live dependents retains LOCK", [] {
        Data d;
        d.bind();
        Environment e;
        auto w = e.writer(d);
        check(!e.root.close_checked() && e.io->live() == 5, "root released beneath writer");
        check(w.advance() == PendingBaseWriteStep::Error &&
                  w.error().root.code == RootErrorCode::DependentsAlive,
              "shared terminal failure");
        terminal(w);
    });
    for (bool last : {false, true}) {
        std::size_t count = 0;
        std::vector<RootIoEvent> healthy_trace;
        {
            Data d;
            d.bind();
            Environment a, b;
            auto target = a.writer(d);
            auto incoming = b.writer(d);
            if (last)
                a.root = RootLease{};
            a.io->reset();
            target = std::move(incoming);
            count = a.io->used;
            for (std::size_t i = 0; i < count; ++i) {
                const auto event = a.io->trace[i].event;
                healthy_trace.push_back(event);
                std::cout << "MOVE_TRACE last=" << last << " ordinal=" << i + 1
                          << " op=" << static_cast<int>(event.operation)
                          << " role=" << static_cast<int>(event.role)
                          << " stage=" << static_cast<int>(event.stage) << '\n';
            }
            check(target.error().code == PendingBaseWriteError::None &&
                      incoming.error().code == PendingBaseWriteError::Closed,
                  "healthy move assignment");
            complete(target);
            check(target.discard_checked(), "healthy cross-root discard");
        }
        move_planned += count;
        for (std::size_t ordinal = 1; ordinal <= count; ++ordinal) {
            Data d;
            d.bind();
            Environment a, b;
            auto target = a.writer(d);
            auto incoming = b.writer(d);
            if (last)
                a.root = RootLease{};
            const auto incoming_bytes = b.disk.inventory();
            const auto incoming_events = b.io->used;
            a.io->reset();
            a.io->target = ordinal;
            target = std::move(incoming);
            check(a.io->reached == 1, "move disposal fault not reached");
            ++move_reached;
            const auto first = target.error().root;
            const auto event = healthy_trace[ordinal - 1];
            check(first.code == RootErrorCode::IoFailure && first.operation == event.operation &&
                      first.role == event.role && first.stage == event.stage &&
                      first.system_errno == EIO,
                  "move target exact first error");
            terminal(target);
            target = std::move(incoming); // Already-failed target must also preserve B.
            check(target.error().root == first, "failed target lost first error");
            check(incoming.error().code == PendingBaseWriteError::None && b.root.valid() &&
                      b.io->used == incoming_events && b.disk.inventory() == incoming_bytes,
                  "failed A disposal consumed/poisoned B");
            complete(incoming);
            check(incoming.discard_checked(), "preserved incoming cannot complete");
            ++move_validated;
            ++move_cases;
            ++scenarios;
            std::cout << "MOVE_INJECTION last=" << last << " ordinal=" << ordinal
                      << " reached=1 validated=1\n";
            a.io->capture = false;
            b.io->capture = false;
        }
    }
    for (auto role : {RootRole::StagedBase, RootRole::StagedTail, RootRole::StagedWatermark,
                      RootRole::LockFile, RootRole::Root})
        scenario("failed close consumes descriptor once with reuse", [=] {
            Data d;
            d.bind();
            Environment e;
            auto w = e.writer(d);
            e.root = RootLease{};
            e.io->close_fault = true;
            e.io->close_role = role;
            e.io->close_stage = RootStage::Call;
            e.io->close_errno = EINTR;
            e.io->reuse_close = true;
            check(!w.close_checked(), "close failure accepted");
            terminal(w);
            check(e.io->close_hits == 1 && e.io->replacement >= 0 &&
                      ::fcntl(e.io->replacement, F_GETFD) >= 0,
                  "close retried reused descriptor");
            check(::close(e.io->replacement) == 0, "replacement cleanup");
            e.io->replacement = -1;
            ++move_cases;
        });
}
// Test allocation hook counts ordinary operator-new requests, not exception ABI storage.
void allocation_campaign() {
    for (bool lru : {false, true})
        for (int kind = 0; kind < 3; ++kind) {
            std::size_t total = 0;
            const auto run = [&](std::size_t fault) {
                const auto live_before = live_allocations;
                {
                    Data d;
                    if (kind == 0)
                        d = sample();
                    else if (kind == 1)
                        d.add("string", storage::StringValue{std::string(300, 'v')});
                    else
                        d.add("list",
                              storage::ListValue{{std::string(100, 'x'), std::string(200, 'y')}});
                    d.bind();
                    const auto before = d.fingerprint();
                    Environment e;
                    auto p = e.pending();
                    e.io->capture = false;
                    allocation_calls = reached = 0;
                    armed = fault;
                    operation = 0;
                    observe_allocations = true;
                    auto w = PendingBaseWriter::begin(std::move(p), d.ctx, d.input(lru), limits());
                    if (w.error().code == PendingBaseWriteError::None) {
                        operation = 1;
                        while (w.advance() == PendingBaseWriteStep::More)
                            ++operation;
                    }
                    observe_allocations = false;
                    armed = 0;
                    if (!fault) {
                        total = allocation_calls;
                        for (std::size_t i = 0; i < total; ++i)
                            std::cout << "ALLOCATION_TRACE kind=" << kind << " lru=" << lru
                                      << " ordinal=" << i + 1
                                      << " bytes=" << allocation_trace[i].size
                                      << " operation=" << allocation_trace[i].operation << '\n';
                        check(w.written_unsealed(), "healthy allocation trace");
                    } else {
                        check(reached == 1, "unreached allocation fault");
                        ++allocation_reached;
                        check(w.error().code == PendingBaseWriteError::Encoding &&
                                  w.error().encoding.code == BaseEncodeError::ResourceLimit,
                              "allocation mapping");
                        terminal(w);
                        check(d.fingerprint() == before, "allocation failure changed input");
                        if (p.valid())
                            check(e.root.valid() && p.discard_checked(),
                                  "pre-transfer allocation stole source");
                        else {
                            check(w.error().root.code == RootErrorCode::ResourceLimit &&
                                      w.error().root.system_errno == 0 &&
                                      w.error().root.operation == RootOperation::None,
                                  "fabricated allocation errno");
                            e.retained_extent();
                        }
                        ++allocation_validated;
                        std::cout << "ALLOCATION kind=" << kind << " lru=" << lru
                                  << " ordinal=" << fault << " reached=1 validated=1 phase="
                                  << (w.error().root.code == RootErrorCode::None ? "pre-transfer"
                                                                                 : "post-transfer")
                                  << " bytes=" << allocation_trace[fault - 1].size
                                  << " operation=" << allocation_trace[fault - 1].operation << '\n';
                    }
                }
                check(live_allocations == live_before, "allocation leak after cleanup");
            };
            run(0);
            allocation_planned += total;
            for (std::size_t ordinal = 1; ordinal <= total; ++ordinal) {
                run(ordinal);
                ++scenarios;
            }
        }
}
void large_stream() {
    scenario("75498000-byte bounded aggregate", [] {
        Data d;
        for (int i = 0; i < 9; ++i)
            d.add(std::string(1, static_cast<char>('a' + i)),
                  storage::StringValue{std::string(MaxLruPayload, 'v')});
        d.bind();
        Environment e;
        auto w = e.writer(d, false);
        e.io->capture = false;
        complete(w);
        check(w.generated()->identity().byte_length == 75498000, "large exact extent");
        e.retained_extent();
        const auto fd = ::open(e.base().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        check(fd >= 0, "large oracle open");
        std::array<char, 65536> buffer{};
        std::uint32_t running = 0;
        std::uint64_t extent = 0;
        for (;;) {
            const auto n = ::read(fd, buffer.data(), buffer.size());
            check(n >= 0, "large oracle read");
            if (!n)
                break;
            const auto size = static_cast<std::size_t>(n);
            running = crc(std::string_view{buffer.data(), size}, running);
            extent += size;
        }
        check(::close(fd) == 0 && extent == 75498000 &&
                  running == w.generated()->identity().file_crc.value(),
              "large streamed CRC/extent");
        check(e.io->max_write == 65536 && e.io->syncs == 0 && e.io->reads == 0,
              "large I/O boundary");
        std::cout << "LARGE_STREAM bytes=" << extent << " max_pwrite=" << e.io->max_write
                  << " crc32c=" << running << " full_base_buffer=0 fsync=0 production_readback=0\n";
        check(w.discard_checked(), "large discard");
    });
}
} // namespace
int main() {
    try {
        success_cases();
        preflight_cases();
        io_campaign();
        write_cases();
        moves();
        allocation_campaign();
        large_stream();
        check(planned == injected && injected == validated, "I/O accountability");
        check(move_planned == move_reached && move_reached == move_validated,
              "move accountability");
        check(allocation_planned == allocation_reached &&
                  allocation_reached == allocation_validated,
              "allocation accountability");
        std::cout << "PASS scenarios=" << scenarios << " io_planned=" << planned
                  << " io_reached=" << injected << " io_validated=" << validated
                  << " allocation_planned=" << allocation_planned
                  << " allocation_reached=" << allocation_reached
                  << " allocation_validated=" << allocation_validated
                  << " move_planned=" << move_planned << " move_reached=" << move_reached
                  << " move_validated=" << move_validated << " move_cases=" << move_cases
                  << " peak_request=" << largest_request << '\n';
        return 0;
    } catch (const std::exception& e) {
        observe_allocations = false;
        std::cerr << "FAIL after " << scenarios << " scenarios: " << e.what() << '\n';
        return 1;
    }
}
