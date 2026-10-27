#include "redisdb/persistence/V2Root.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace redisdb::persistence::v2 {
int RootIo::open(const char* path, int flags) noexcept { return ::open(path, flags); }
int RootIo::open_at(int directory, const char* name, int flags) noexcept {
    return ::openat(directory, name, flags);
}
int RootIo::stat(int fd, struct stat* info) noexcept { return ::fstat(fd, info); }
int RootIo::named_stat(int directory, const char* name, struct stat* info, int flags) noexcept {
    return ::fstatat(directory, name, info, flags);
}
int RootIo::lock(int fd, int flags) noexcept { return ::flock(fd, flags); }
ssize_t RootIo::read(int fd, void* bytes, std::size_t count, off_t offset) noexcept {
    return ::pread(fd, bytes, count, offset);
}
int RootIo::close(int fd) noexcept { return ::close(fd); }
int RootIo::observe(const RootIoEvent&) noexcept { return 0; }

namespace {
constexpr int ReadFlags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
constexpr auto OffsetCeiling = std::min<std::uint64_t>(
    MaxFileBytes, static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()));
static_assert(std::numeric_limits<off_t>::is_signed && sizeof(off_t) <= sizeof(std::int64_t));
static_assert(sizeof(ssize_t) <= sizeof(std::int64_t));
static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
RootError closed_error() noexcept { return {RootErrorCode::ClosedOrFailed}; }
std::array<char, 33> fixed_name(const char* text) noexcept {
    std::array<char, 33> name{};
    std::memcpy(name.data(), text, std::strlen(text));
    return name;
}
} // namespace

namespace detail {
struct RootState {
    std::shared_ptr<RootIo> io;
    int directory = -1, lock_fd = -1;
    FileIdentity root_identity, lock_identity;
    RootError failure;
    bool closed = false;

    explicit RootState(std::shared_ptr<RootIo> adapter) noexcept : io(std::move(adapter)) {}
    ~RootState() noexcept {
        (void)close_fd(lock_fd, RootRole::LockFile);
        (void)close_fd(directory, RootRole::Root);
    }
    bool healthy() const noexcept { return !closed && failure.code == RootErrorCode::None; }
    bool fail(RootErrorCode code, RootOperation op, RootRole role, int err = 0,
              RootStage stage = RootStage::Call) noexcept {
        if (failure.code == RootErrorCode::None)
            failure = {code, op, role, stage, err};
        return false;
    }
    RootErrorCode syscall_error(RootOperation op, RootRole role, int err) const noexcept {
        if (op == RootOperation::Lock)
            return RootErrorCode::LockUnavailable;
        if (op == RootOperation::Open && role == RootRole::Root)
            return RootErrorCode::InvalidRoot;
        if (op == RootOperation::OpenAt && role == RootRole::LockFile && err == ENOENT)
            return RootErrorCode::MissingLock;
        return RootErrorCode::IoFailure;
    }
    // The caller adopts an open result inside fn, before the After hook can fail.
    // Only actual EINTR results retry; observation-hook failures are terminal.
    template <class Fn>
    bool call(RootOperation op, RootRole role, Fn fn, std::int64_t& result,
              bool opening = false) noexcept {
        for (;;) {
            const auto before = io->observe({op, role, RootStage::Before, 0, 0});
            if (before != 0)
                return fail(RootErrorCode::IoFailure, op, role, before > 0 ? before : EIO,
                            RootStage::Before);
            errno = 0;
            result = fn();
            const auto saved = result < 0 ? errno : 0;
            const auto after = io->observe({op, role, RootStage::After, result, saved});
            if (result == -1 && saved <= 0)
                return fail(RootErrorCode::IoFailure, op, role, EIO);
            if (result == -1 && saved != EINTR)
                return fail(syscall_error(op, role, saved), op, role, saved);
            if (result < -1 ||
                (result >= 0 && !opening && op != RootOperation::Read && result != 0))
                return fail(RootErrorCode::IoFailure, op, role, EIO);
            if (after != 0)
                return fail(RootErrorCode::IoFailure, op, role, after > 0 ? after : EIO,
                            RootStage::After);
            if (result == -1) {
                if (saved == EINTR)
                    continue;
                return fail(RootErrorCode::IoFailure, op, role, saved);
            }
            return true;
        }
    }
    bool close_fd(int& fd, RootRole role) noexcept {
        if (fd < 0)
            return true;
        const auto owned = std::exchange(fd, -1); // Never retry a relinquished number.
        const auto before = io->observe({RootOperation::Close, role, RootStage::Before, 0, 0});
        errno = 0;
        const auto result = io->close(owned);
        const auto saved = result < 0 ? errno : 0;
        const auto after =
            io->observe({RootOperation::Close, role, RootStage::After, result, saved});
        if (before != 0)
            return fail(RootErrorCode::IoFailure, RootOperation::Close, role, before,
                        RootStage::Before);
        if (result != 0)
            return fail(RootErrorCode::IoFailure, RootOperation::Close, role,
                        result == -1 ? saved : EIO);
        if (after != 0)
            return fail(RootErrorCode::IoFailure, RootOperation::Close, role, after,
                        RootStage::After);
        return true;
    }
    bool metadata(const struct stat& info, RootRole role, FileIdentity& identity,
                  RootOperation op) noexcept {
        if (role == RootRole::Root) {
            if (!S_ISDIR(info.st_mode))
                return fail(RootErrorCode::InvalidRoot, op, role);
            identity = {info.st_dev, info.st_ino, 0}; // Directory size is not a stable identity.
            return true;
        }
        if (!S_ISREG(info.st_mode))
            return fail(RootErrorCode::InvalidFileType, op, role);
        if (info.st_nlink != 1)
            return fail(RootErrorCode::LinkCount, op, role);
        if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > OffsetCeiling)
            return fail(RootErrorCode::InvalidLength, op, role);
        const auto size = static_cast<std::uint64_t>(info.st_size);
        if (((role == RootRole::Current || role == RootRole::Watermark) && size != 112) ||
            (role == RootRole::Base && size < 204) || (role == RootRole::Tail && size < 96))
            return fail(RootErrorCode::InvalidLength, op, role);
        identity = {info.st_dev, info.st_ino, size};
        return true;
    }
    bool inspect(int fd, RootRole role, FileIdentity& identity) noexcept {
        struct stat info{};
        std::int64_t result = 0;
        return call(
                   RootOperation::Stat, role, [&] { return io->stat(fd, &info); }, result) &&
               metadata(info, role, identity, RootOperation::Stat);
    }
    bool named(const char* name, RootRole role, const FileIdentity& expected) noexcept {
        struct stat info{};
        std::int64_t result = 0;
        if (!call(
                RootOperation::NamedStat, role,
                [&] { return io->named_stat(directory, name, &info, AT_SYMLINK_NOFOLLOW); },
                result))
            return false;
        FileIdentity identity;
        return metadata(info, role, identity, RootOperation::NamedStat) &&
               (identity == expected ||
                fail(RootErrorCode::IdentityChanged, RootOperation::NamedStat, role));
    }
    bool check_root() noexcept {
        if (!healthy())
            return false;
        FileIdentity root, lock_identity_now;
        return inspect(directory, RootRole::Root, root) &&
               (root == root_identity ||
                fail(RootErrorCode::IdentityChanged, RootOperation::Stat, RootRole::Root)) &&
               inspect(lock_fd, RootRole::LockFile, lock_identity_now) &&
               (lock_identity_now == lock_identity ||
                fail(RootErrorCode::IdentityChanged, RootOperation::Stat, RootRole::LockFile)) &&
               named("LOCK", RootRole::LockFile, lock_identity);
    }
};
} // namespace detail

RootLease RootLease::open_existing(const std::filesystem::path& root,
                                   std::shared_ptr<RootIo> io) noexcept {
    RootLease owner;
    try {
        if (!io)
            io = std::make_shared<RootIo>();
        owner.state_ = std::make_shared<detail::RootState>(std::move(io));
        auto& state = *owner.state_;
        const auto acquire = [&] {
            if (root.empty() || root.native().find('\0') != std::string::npos) {
                (void)state.fail(RootErrorCode::InvalidRoot, RootOperation::Open, RootRole::Root);
                return;
            }
            auto path = root;
            // Strip only terminal slash/dot components, as accepted V1 does. Keep
            // trusted ancestors and '..' semantics; no canonicalization/realpath.
            while (path.has_relative_path() && path != "." &&
                   (path.filename().empty() || path.filename() == ".")) {
                const auto parent = path.parent_path();
                path = parent.empty() ? std::filesystem::path{"."} : parent;
            }
            std::int64_t result = 0;
            if (!state.call(
                    RootOperation::Open, RootRole::Root,
                    [&] {
                        state.directory = state.io->open(path.c_str(), O_RDONLY | O_DIRECTORY |
                                                                           O_CLOEXEC | O_NOFOLLOW);
                        return state.directory;
                    },
                    result, true) ||
                !state.inspect(state.directory, RootRole::Root, state.root_identity))
                return;
            if (!state.call(
                    RootOperation::OpenAt, RootRole::LockFile,
                    [&] {
                        state.lock_fd = state.io->open_at(state.directory, "LOCK", ReadFlags);
                        return state.lock_fd;
                    },
                    result, true) ||
                !state.inspect(state.lock_fd, RootRole::LockFile, state.lock_identity) ||
                !state.call(
                    RootOperation::Lock, RootRole::LockFile,
                    [&] { return state.io->lock(state.lock_fd, LOCK_EX | LOCK_NB); }, result))
                return;
            (void)state.check_root(); // Named LOCK identity checked AFTER acquiring flock.
        };
        acquire();
    } catch (const std::bad_alloc&) {
        owner.local_error_ = {RootErrorCode::ResourceLimit};
        if (owner.state_)
            (void)owner.state_->fail(RootErrorCode::ResourceLimit, RootOperation::None,
                                     RootRole::Root);
    } catch (const std::length_error&) {
        owner.local_error_ = {RootErrorCode::ResourceLimit};
        if (owner.state_)
            (void)owner.state_->fail(RootErrorCode::ResourceLimit, RootOperation::None,
                                     RootRole::Root);
    }
    // Failed acquisition releases partial FDs immediately, not at owner destruction.
    if (owner.state_ && !owner.state_->healthy()) {
        (void)owner.state_->close_fd(owner.state_->lock_fd, RootRole::LockFile);
        (void)owner.state_->close_fd(owner.state_->directory, RootRole::Root);
    }
    return owner;
}
bool RootLease::valid() const noexcept { return state_ && state_->healthy(); }
RootError RootLease::error() const noexcept {
    return state_ ? (state_->healthy()                             ? RootError{}
                     : state_->failure.code == RootErrorCode::None ? closed_error()
                                                                   : state_->failure)
                  : local_error_;
}
bool RootLease::recheck() noexcept { return state_ && state_->check_root(); }
PinnedFile RootLease::pin(RootRole role, const std::array<char, 33>& name) noexcept {
    PinnedFile file;
    file.state_ = state_;
    if (!recheck())
        return file;
    file.role_ = role;
    file.name_ = name;
    std::int64_t result = 0;
    if (!state_->call(
            RootOperation::OpenAt, role,
            [&] {
                file.fd_ = state_->io->open_at(state_->directory, name.data(), ReadFlags);
                return file.fd_;
            },
            result, true) ||
        !state_->inspect(file.fd_, role, file.identity_) ||
        !state_->named(name.data(), role, file.identity_) || !state_->check_root())
        (void)state_->close_fd(file.fd_, role);
    return file;
}
PinnedFile RootLease::pin_current() noexcept {
    return pin(RootRole::Current, fixed_name("CURRENT"));
}
PinnedFile RootLease::pin_generation_file(ArtifactRole role, GenerationId generation) noexcept {
    if (!valid())
        return {};
    const char *prefix = nullptr, *suffix = nullptr;
    RootRole file_role;
    switch (role) {
    case ArtifactRole::Base:
        prefix = "base-";
        suffix = ".snap";
        file_role = RootRole::Base;
        break;
    case ArtifactRole::Tail:
        prefix = "tail-";
        suffix = ".aof";
        file_role = RootRole::Tail;
        break;
    case ArtifactRole::Watermark:
        prefix = "tail-";
        suffix = ".commit";
        file_role = RootRole::Watermark;
        break;
    default:
        (void)state_->fail(RootErrorCode::InvalidName, RootOperation::OpenAt, RootRole::Root);
        return {};
    }
    auto name = fixed_name(prefix);
    auto* start = name.data() + std::strlen(prefix);
    const auto rendered = std::to_chars(start, name.data() + 25, generation.value());
    if (rendered.ec != std::errc{}) {
        (void)state_->fail(RootErrorCode::InvalidName, RootOperation::OpenAt, file_role);
        return {};
    }
    const auto suffix_size = std::strlen(suffix);
    std::uint64_t parsed = 0;
    const auto roundtrip = std::from_chars(start, rendered.ptr, parsed);
    if (roundtrip.ec != std::errc{} || roundtrip.ptr != rendered.ptr ||
        parsed != generation.value() ||
        suffix_size >= static_cast<std::size_t>(name.data() + name.size() - rendered.ptr)) {
        (void)state_->fail(RootErrorCode::InvalidName, RootOperation::OpenAt, file_role);
        return {};
    }
    std::memcpy(rendered.ptr, suffix, suffix_size + 1);
    return pin(file_role, name);
}
bool RootLease::close_checked() noexcept {
    if (!valid())
        return false;
    if (state_.use_count() != 1)
        return state_->fail(RootErrorCode::DependentsAlive, RootOperation::Close, RootRole::Root);
    const auto lock_ok = state_->close_fd(state_->lock_fd, RootRole::LockFile);
    const auto root_ok = state_->close_fd(state_->directory, RootRole::Root);
    state_->closed = true;
    return lock_ok && root_ok;
}

PinnedFile::~PinnedFile() noexcept { dispose(); }
void PinnedFile::dispose() noexcept {
    if (state_)
        (void)state_->close_fd(fd_, role_);
    state_.reset();
}
PinnedFile::PinnedFile(PinnedFile&& other) noexcept
    : state_(std::move(other.state_)), fd_(std::exchange(other.fd_, -1)), role_(other.role_),
      name_(other.name_), identity_(other.identity_) {}
PinnedFile& PinnedFile::operator=(PinnedFile&& other) noexcept {
    if (this != &other) {
        dispose();
        state_ = std::move(other.state_);
        fd_ = std::exchange(other.fd_, -1);
        role_ = other.role_;
        name_ = other.name_;
        identity_ = other.identity_;
    }
    return *this;
}
bool PinnedFile::valid() const noexcept { return state_ && state_->healthy() && fd_ >= 0; }
RootError PinnedFile::error() const noexcept {
    return state_ && state_->failure.code != RootErrorCode::None ? state_->failure
           : valid()                                             ? RootError{}
                                                                 : closed_error();
}
const FileIdentity* PinnedFile::identity() const noexcept { return valid() ? &identity_ : nullptr; }
bool PinnedFile::recheck() noexcept {
    if (!valid() || !state_->check_root())
        return false;
    FileIdentity now;
    return state_->inspect(fd_, role_, now) &&
           (now == identity_ ||
            state_->fail(RootErrorCode::IdentityChanged, RootOperation::Stat, role_)) &&
           state_->named(name_.data(), role_, identity_);
}
bool PinnedFile::read_exact(std::uint64_t offset, std::span<std::byte> output) noexcept {
    if (!recheck())
        return false;
    if (offset > identity_.byte_length || output.size() > identity_.byte_length - offset)
        return state_->fail(RootErrorCode::InvalidLength, RootOperation::Read, role_);
    while (!output.empty()) {
        const auto count = std::min<std::size_t>(
            {output.size(), 65536, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())});
        std::int64_t result = 0;
        if (!state_->call(
                RootOperation::Read, role_,
                [&] {
                    return state_->io->read(fd_, output.data(), count, static_cast<off_t>(offset));
                },
                result))
            return false;
        if (result == 0)
            return state_->fail(RootErrorCode::Truncated, RootOperation::Read, role_);
        if (static_cast<std::uint64_t>(result) > count)
            return state_->fail(RootErrorCode::IoFailure, RootOperation::Read, role_, EIO);
        const auto n = static_cast<std::size_t>(result);
        offset += n; // Proved <= captured extent <= INT64_MAX/off_t ceiling above.
        output = output.subspan(n);
    }
    return recheck();
}
bool PinnedFile::close_checked() noexcept {
    if (!valid())
        return false;
    return state_->close_fd(fd_, role_);
}
} // namespace redisdb::persistence::v2
