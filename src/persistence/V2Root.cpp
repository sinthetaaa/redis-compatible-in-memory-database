#include "redisdb/persistence/V2Root.hpp"
#include "V2GenerationAccess.hpp"
#include "V2PendingAccess.hpp"
#include "V2RootAccess.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/file.h>
#include <sys/random.h>
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
int RootIo::create_at(int directory, const char* name, int flags, mode_t mode) noexcept {
    return ::openat(directory, name, flags, mode);
}
int RootIo::random_128(std::array<std::byte, 16>& token) noexcept {
    return ::getentropy(token.data(), token.size());
}
ssize_t RootIo::write_at(int fd, const void* bytes, std::size_t count, off_t offset) noexcept {
    return ::pwrite(fd, bytes, count, offset);
}
int RootIo::sync_file(int fd) noexcept { return ::fsync(fd); }
int RootIo::unlink_at(int directory, const char* name) noexcept {
    return ::unlinkat(directory, name, 0);
}
int RootIo::close(int fd) noexcept { return ::close(fd); }
int RootIo::observe(const RootIoEvent&) noexcept { return 0; }

namespace {
enum class Presence { Absent, Occupied, Failed };
constexpr std::array<RootRole, 3> CandidateRoles{RootRole::Base, RootRole::Tail,
                                                 RootRole::Watermark};
bool candidate_names(GenerationId generation, std::array<std::array<char, 33>, 3>& names,
                     RootRole& role) noexcept {
    constexpr std::array<const char*, 3> prefixes{"base-", "tail-", "tail-"};
    constexpr std::array<const char*, 3> suffixes{".snap", ".aof", ".commit"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        role = CandidateRoles[i];
        auto& name = names[i];
        name.fill('\0');
        std::memcpy(name.data(), prefixes[i], 5);
        auto* begin = name.data() + 5;
        const auto rendered = std::to_chars(begin, begin + 20, generation.value());
        if (generation.value() == 0 || rendered.ec != std::errc{} || rendered.ptr == begin ||
            *begin == '0')
            return false;
        std::uint64_t parsed = 0;
        const auto roundtrip = std::from_chars(begin, rendered.ptr, parsed);
        const auto suffix_size = std::strlen(suffixes[i]);
        const auto length = static_cast<std::size_t>(rendered.ptr - name.data());
        if (roundtrip.ec != std::errc{} || roundtrip.ptr != rendered.ptr ||
            parsed != generation.value() || length >= name.size() ||
            suffix_size >= name.size() - length)
            return false;
        std::memcpy(rendered.ptr, suffixes[i], suffix_size + 1);
    }
    return true;
}
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
    bool pending_generation_active = false;

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
    // Count returns are distinct from fixed-result calls. Commit successful write
    // progress BEFORE observing After, so a rejecting observer cannot replay it.
    bool staging_write(int fd, RootRole role, const void* input, std::size_t count,
                       std::uint64_t offset, std::size_t& progress, FileIdentity& extent) noexcept {
        constexpr auto op = RootOperation::WriteAt;
        for (;;) {
            const auto before = io->observe({op, role, RootStage::Before, 0, 0});
            if (before != 0)
                return fail(RootErrorCode::IoFailure, op, role, before > 0 ? before : EIO,
                            RootStage::Before);
            errno = 0;
            const auto result = io->write_at(fd, input, count, static_cast<off_t>(offset));
            const auto saved = result < 0 ? errno : 0;
            const auto valid_count = result > 0 && static_cast<std::uint64_t>(result) <= count;
            if (valid_count) {
                progress = static_cast<std::size_t>(result);
                extent.byte_length += progress;
            }
            const auto after = io->observe({op, role, RootStage::After, result, saved});
            if (result < -1 || (result > 0 && !valid_count) || (result == -1 && saved <= 0))
                return fail(RootErrorCode::IoFailure, op, role, EIO);
            if (result == -1 && saved != EINTR)
                return fail(RootErrorCode::IoFailure, op, role, saved);
            if (after != 0)
                return fail(RootErrorCode::IoFailure, op, role, after > 0 ? after : EIO,
                            RootStage::After);
            if (result == -1)
                continue;
            if (result == 0)
                return fail(RootErrorCode::IoFailure, op, role, EIO);
            return true;
        }
    }
    bool staging_read(int fd, RootRole role, void* output, std::size_t count, std::uint64_t offset,
                      std::size_t& progress) noexcept {
        constexpr auto op = RootOperation::Read;
        for (;;) {
            const auto before = io->observe({op, role, RootStage::Before, 0, 0});
            if (before != 0)
                return fail(RootErrorCode::IoFailure, op, role, before > 0 ? before : EIO,
                            RootStage::Before);
            errno = 0;
            const auto result = io->read(fd, output, count, static_cast<off_t>(offset));
            const auto saved = result < 0 ? errno : 0;
            const auto valid_count = result > 0 && static_cast<std::uint64_t>(result) <= count;
            if (valid_count)
                progress = static_cast<std::size_t>(result);
            const auto after = io->observe({op, role, RootStage::After, result, saved});
            if (result < -1 || (result > 0 && !valid_count) || (result == -1 && saved <= 0))
                return fail(RootErrorCode::IoFailure, op, role, EIO);
            if (result == -1 && saved != EINTR)
                return fail(RootErrorCode::IoFailure, op, role, saved);
            if (after != 0)
                return fail(RootErrorCode::IoFailure, op, role, after > 0 ? after : EIO,
                            RootStage::After);
            if (result == -1)
                continue;
            if (result == 0)
                return fail(RootErrorCode::Truncated, op, role);
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
    // Expected absence belongs only to candidate occupancy, never selected pins
    // or LOCK. No metadata/type/size validation of an occupied entry is needed.
    Presence candidate_presence(const char* name, RootRole role) noexcept {
        constexpr auto op = RootOperation::NamedStat;
        for (;;) {
            const auto before = io->observe({op, role, RootStage::Before, 0, 0});
            if (before != 0) {
                (void)fail(RootErrorCode::IoFailure, op, role, before > 0 ? before : EIO,
                           RootStage::Before);
                return Presence::Failed;
            }
            struct stat info{};
            errno = 0;
            const auto result = io->named_stat(directory, name, &info, AT_SYMLINK_NOFOLLOW);
            const auto saved = result < 0 ? errno : 0;
            const auto after = io->observe({op, role, RootStage::After, result, saved});
            if (result < -1 || result > 0 || (result == -1 && saved <= 0)) {
                (void)fail(RootErrorCode::IoFailure, op, role, EIO);
                return Presence::Failed;
            }
            if (result == -1 && saved != ENOENT && saved != EINTR) {
                (void)fail(RootErrorCode::IoFailure, op, role, saved);
                return Presence::Failed;
            }
            if (after != 0) {
                (void)fail(RootErrorCode::IoFailure, op, role, after > 0 ? after : EIO,
                           RootStage::After);
                return Presence::Failed;
            }
            if (result == -1 && saved == EINTR)
                continue;
            return result == 0 ? Presence::Occupied : Presence::Absent;
        }
    }
    Presence candidate_triple(GenerationId generation, RootRole& occupied_role) noexcept {
        if (!check_root())
            return Presence::Failed;
        std::array<std::array<char, 33>, 3> names{};
        RootRole role = RootRole::Base;
        if (!candidate_names(generation, names, role)) {
            (void)fail(RootErrorCode::InvalidName, RootOperation::NamedStat, role);
            return Presence::Failed;
        }
        auto presence = Presence::Absent;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto observed = candidate_presence(names[i].data(), CandidateRoles[i]);
            if (observed == Presence::Failed)
                return observed;
            if (observed == Presence::Occupied && presence == Presence::Absent) {
                presence = observed;
                occupied_role = CandidateRoles[i];
            }
        }
        return check_root() ? presence : Presence::Failed;
    }
};

void GenerationAccess::borrow(GenerationCandidate& candidate, RootLease& root) noexcept {
    candidate.root_ = root.state_;
    if (!candidate.root_)
        candidate.local_error_ = root.error();
}
bool GenerationAccess::healthy(const GenerationCandidate& candidate) noexcept {
    return candidate.root_ && candidate.root_->healthy();
}
RootError GenerationAccess::error(const GenerationCandidate& candidate) noexcept {
    return candidate.root_ ? candidate.root_->failure : closed_error();
}
void GenerationAccess::search(GenerationCandidate& candidate, GenerationId selected,
                              LogicalSequence sequence, std::uint32_t budget) noexcept {
    if (!healthy(candidate))
        return;
    auto& root = *candidate.root_;
    if (!root.check_root())
        return;
    if (sequence.value() == UINT64_MAX) {
        (void)root.fail(RootErrorCode::SequenceExhausted, RootOperation::None, RootRole::Root);
        return;
    }
    if (selected.value() == UINT64_MAX) {
        (void)root.fail(RootErrorCode::GenerationExhausted, RootOperation::None, RootRole::Root);
        return;
    }
    auto next = selected.value() + 1; // selected < UINT64_MAX proved above.
    for (std::uint32_t inspected = 0; inspected < budget; ++inspected) {
        const GenerationId generation{next};
        RootRole occupied_role = RootRole::Base;
        const auto presence = root.candidate_triple(generation, occupied_role);
        if (presence == Presence::Failed)
            return;
        if (presence == Presence::Absent) {
            const auto confirmation = root.candidate_triple(generation, occupied_role);
            if (confirmation == Presence::Absent)
                candidate.generation_ = generation;
            else if (confirmation == Presence::Occupied)
                (void)root.fail(RootErrorCode::CandidateOccupied, RootOperation::NamedStat,
                                occupied_role);
            return;
        }
        if (next == UINT64_MAX) {
            (void)root.fail(RootErrorCode::GenerationExhausted, RootOperation::None,
                            RootRole::Root);
            return;
        }
        if (inspected == budget - 1) {
            (void)root.fail(RootErrorCode::ResourceLimit, RootOperation::None, RootRole::Root);
            return;
        }
        ++next; // occupied maximum returned above; never wrap to zero.
    }
}
bool GenerationAccess::check(GenerationCandidate& candidate) noexcept {
    if (!candidate.valid())
        return false;
    RootRole role = RootRole::Base;
    auto& root = *candidate.root_;
    const auto presence = root.candidate_triple(*candidate.generation_, role);
    return presence == Presence::Absent ||
           (presence == Presence::Occupied &&
            root.fail(RootErrorCode::CandidateOccupied, RootOperation::NamedStat, role));
}
RootError GenerationAccess::release(GenerationCandidate& candidate) noexcept {
    RootError error;
    if (candidate.root_) {
        auto& root = *candidate.root_;
        // Capture operational close failure before releasing the final state.
        // RootState's destructor sees consumed descriptors and cannot retry them.
        if (candidate.root_.use_count() == 1) {
            (void)root.close_fd(root.lock_fd, RootRole::LockFile);
            (void)root.close_fd(root.directory, RootRole::Root);
            root.closed = true;
        }
        error = root.failure;
        candidate.root_.reset();
    }
    candidate.generation_.reset();
    return error;
}

namespace {
RootRole staging_role(ArtifactRole role) noexcept {
    switch (role) {
    case ArtifactRole::Base:
        return RootRole::StagedBase;
    case ArtifactRole::Tail:
        return RootRole::StagedTail;
    case ArtifactRole::Watermark:
        return RootRole::StagedWatermark;
    }
    return RootRole::Root;
}
bool staging_prefix(ArtifactRole role, GenerationId generation, std::array<char, 70>& name,
                    std::size_t& length) noexcept {
    const char* prefix = nullptr;
    const char* suffix = nullptr;
    switch (role) {
    case ArtifactRole::Base:
        prefix = "base-";
        suffix = ".snap.tmp.";
        break;
    case ArtifactRole::Tail:
        prefix = "tail-";
        suffix = ".aof.tmp.";
        break;
    case ArtifactRole::Watermark:
        prefix = "tail-";
        suffix = ".commit.tmp.";
        break;
    default:
        return false;
    }
    if (generation.value() == 0)
        return false;
    name.fill('\0');
    std::memcpy(name.data(), prefix, 5);
    const auto rendered = std::to_chars(name.data() + 5, name.data() + 25, generation.value());
    if (rendered.ec != std::errc{})
        return false;
    std::uint64_t parsed = 0;
    const auto roundtrip = std::from_chars(name.data() + 5, rendered.ptr, parsed);
    if (roundtrip.ec != std::errc{} || roundtrip.ptr != rendered.ptr ||
        parsed != generation.value())
        return false;
    length = static_cast<std::size_t>(rendered.ptr - name.data());
    const auto n = std::strlen(suffix);
    if (length > 37 || n > 37 - length)
        return false;
    std::memcpy(name.data() + length, suffix, n);
    length += n;
    return true;
}
bool staging_name(ArtifactRole role, GenerationId generation,
                  const std::array<char, 70>& name) noexcept {
    std::array<char, 70> expected{};
    std::size_t prefix = 0;
    if (!staging_prefix(role, generation, expected, prefix) ||
        std::memcmp(name.data(), expected.data(), prefix) != 0 || name[prefix + 32] != '\0')
        return false;
    for (std::size_t i = prefix; i < prefix + 32; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
            return false;
    return true;
}
} // namespace
void StagingAccess::borrow(StagedFile& file, RootLease& root) noexcept {
    file.root_ = root.state_;
    if (!file.root_)
        file.local_error_ = root.error();
}
bool StagingAccess::healthy(const StagedFile& file) noexcept {
    return file.root_ && file.root_->healthy();
}
RootError StagingAccess::error(const StagedFile& file) noexcept {
    return file.root_ ? file.root_->failure : closed_error();
}
bool StagingAccess::fail(StagedFile& file, RootErrorCode code, RootOperation op) noexcept {
    return file.root_ && file.root_->fail(code, op, staging_role(file.role_));
}
bool StagingAccess::create(StagedFile& file) noexcept {
    if (!healthy(file))
        return false;
    auto& root = *file.root_;
    const auto role = staging_role(file.role_);
    std::size_t prefix = 0;
    if (!staging_prefix(file.role_, file.generation_, file.name_, prefix))
        return fail(file, RootErrorCode::InvalidName, RootOperation::CreateAt);
    if (!root.check_root())
        return false;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        std::array<std::byte, 16> token{};
        std::int64_t result = 0;
        if (!root.call(
                RootOperation::Random128, role, [&] { return root.io->random_128(token); }, result))
            return false;
        constexpr char digits[] = "0123456789abcdef";
        for (std::size_t i = 0; i < token.size(); ++i) {
            const auto n = std::to_integer<unsigned>(token[i]);
            file.name_[prefix + i * 2] = digits[n >> 4];
            file.name_[prefix + i * 2 + 1] = digits[n & 15];
        }
        if (!staging_name(file.role_, file.generation_, file.name_))
            return fail(file, RootErrorCode::InvalidName, RootOperation::CreateAt);
        const auto op = RootOperation::CreateAt;
        const auto before = root.io->observe({op, role, RootStage::Before, 0, 0});
        if (before != 0)
            return root.fail(RootErrorCode::IoFailure, op, role, before > 0 ? before : EIO,
                             RootStage::Before);
        errno = 0;
        // Adopt the returned descriptor before running any observer.
        file.fd_ = root.io->create_at(
            root.directory, file.name_.data(),
            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        const auto saved = file.fd_ < 0 ? errno : 0;
        if (file.fd_ >= 0)
            file.linked_ = true;
        const auto after = root.io->observe({op, role, RootStage::After, file.fd_, saved});
        if (file.fd_ < -1 || (file.fd_ == -1 && saved <= 0))
            return root.fail(RootErrorCode::IoFailure, op, role, EIO);
        if (file.fd_ == -1 && saved != EEXIST && saved != EINTR)
            return root.fail(RootErrorCode::IoFailure, op, role, saved);
        if (after != 0)
            return root.fail(RootErrorCode::IoFailure, op, role, after > 0 ? after : EIO,
                             RootStage::After);
        if (file.fd_ == -1)
            continue; // Fresh OS token, including ambiguous actual EINTR.
        if (!root.inspect(file.fd_, role, file.identity_) || file.identity_.byte_length != 0) {
            if (root.healthy())
                (void)root.fail(RootErrorCode::InvalidLength, RootOperation::Stat, role);
            return false;
        }
        file.state_ = StagedFile::State::Building;
        return check(file);
    }
    return root.fail(RootErrorCode::CollisionLimit, RootOperation::CreateAt, role, EEXIST);
}
bool StagingAccess::check(StagedFile& file) noexcept {
    if (!file.valid())
        return false;
    if (!file.linked_ || !staging_name(file.role_, file.generation_, file.name_))
        return fail(file, RootErrorCode::InvalidName, RootOperation::NamedStat);
    auto& root = *file.root_;
    const auto role = staging_role(file.role_);
    FileIdentity now;
    return root.check_root() && root.inspect(file.fd_, role, now) &&
           (now == file.identity_ ||
            root.fail(RootErrorCode::IdentityChanged, RootOperation::Stat, role)) &&
           root.named(file.name_.data(), role, file.identity_);
}
bool StagingAccess::append(StagedFile& file, ByteView bytes) noexcept {
    if (!check(file))
        return false;
    if (file.identity_.byte_length > OffsetCeiling ||
        bytes.size() > OffsetCeiling - file.identity_.byte_length)
        return fail(file, RootErrorCode::InvalidLength, RootOperation::WriteAt);
    auto& root = *file.root_;
    while (!bytes.empty()) {
        const auto count = std::min<std::size_t>(
            {bytes.size(), 65536, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())});
        std::size_t progress = 0;
        if (!root.staging_write(file.fd_, staging_role(file.role_), bytes.data(), count,
                                file.identity_.byte_length, progress, file.identity_))
            return false;
        bytes = bytes.subspan(progress);
    }
    return check(file);
}
bool StagingAccess::read(StagedFile& file, std::uint64_t offset,
                         std::span<std::byte> output) noexcept {
    if (!check(file))
        return false;
    if (offset > file.identity_.byte_length || output.size() > file.identity_.byte_length - offset)
        return fail(file, RootErrorCode::InvalidLength, RootOperation::Read);
    auto& root = *file.root_;
    while (!output.empty()) {
        const auto count = std::min<std::size_t>(
            {output.size(), 65536, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())});
        std::size_t progress = 0;
        if (!root.staging_read(file.fd_, staging_role(file.role_), output.data(), count, offset,
                               progress))
            return false;
        offset += progress;
        output = output.subspan(progress);
    }
    return check(file);
}
bool StagingAccess::sync(StagedFile& file) noexcept {
    if (!file.valid())
        return false;
    std::int64_t result = 0;
    return file.root_->call(
        RootOperation::SyncFile, staging_role(file.role_),
        [&] { return file.root_->io->sync_file(file.fd_); }, result);
}
bool StagingAccess::unlink(StagedFile& file) noexcept {
    if (!check(file))
        return false;
    auto& root = *file.root_;
    const auto role = staging_role(file.role_);
    const auto op = RootOperation::UnlinkAt;
    const auto before = root.io->observe({op, role, RootStage::Before, 0, 0});
    if (before != 0)
        return root.fail(RootErrorCode::IoFailure, op, role, before > 0 ? before : EIO,
                         RootStage::Before);
    errno = 0;
    const auto result = root.io->unlink_at(root.directory, file.name_.data());
    const auto saved = result < 0 ? errno : 0;
    if (result == 0)
        file.linked_ = false; // Record irreversible success BEFORE After.
    const auto after = root.io->observe({op, role, RootStage::After, result, saved});
    if (result != 0)
        return root.fail(RootErrorCode::IoFailure, op, role,
                         result == -1 && saved > 0 ? saved : EIO);
    if (after != 0)
        return root.fail(RootErrorCode::IoFailure, op, role, after > 0 ? after : EIO,
                         RootStage::After);
    return root.check_root();
}
bool StagingAccess::close(StagedFile& file) noexcept {
    file.state_ = StagedFile::State::Closed;
    return !file.root_ || file.root_->close_fd(file.fd_, staging_role(file.role_));
}
bool PendingAccess::claim(PendingGeneration& owner, RootLease& lease) noexcept {
    owner.root_ = lease.state_;
    if (!owner.root_ || !owner.root_->healthy()) {
        owner.local_error_ = lease.error();
        owner.root_.reset();
        return false;
    }
    if (owner.root_->pending_generation_active) {
        owner.local_error_ = {RootErrorCode::WrongState}; // Local rejection, no root latch.
        owner.root_.reset();
        return false;
    }
    owner.root_->pending_generation_active = true;
    owner.owns_claim_ = true;
    return true;
}
bool PendingAccess::healthy(const PendingGeneration& owner) noexcept {
    return owner.root_ && owner.owns_claim_ && owner.root_->pending_generation_active &&
           owner.root_->healthy();
}
RootError PendingAccess::error(const PendingGeneration& owner) noexcept {
    return owner.root_ ? owner.root_->failure : closed_error();
}
bool PendingAccess::fail(PendingGeneration& owner, RootErrorCode code, RootOperation operation,
                         RootRole role) noexcept {
    return owner.root_ && owner.root_->fail(code, operation, role);
}
RootError PendingAccess::release(PendingGeneration& owner) noexcept {
    RootError error;
    if (owner.root_) {
        auto& root = *owner.root_;
        if (owner.owns_claim_) {
            root.pending_generation_active = false;
            owner.owns_claim_ = false;
        }
        if (owner.root_.use_count() == 1) {
            (void)root.close_fd(root.lock_fd, RootRole::LockFile);
            (void)root.close_fd(root.directory, RootRole::Root);
            root.closed = true;
        }
        error = root.failure;
        owner.root_.reset();
    }
    return error;
}

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
