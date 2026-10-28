#pragma once

#include "redisdb/persistence/V2Format.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <sys/stat.h>
#include <sys/types.h>

namespace redisdb::persistence::v2 {

enum class RootErrorCode {
    None,
    InvalidRoot,
    MissingLock,
    InvalidFileType,
    LinkCount,
    LockUnavailable,
    IdentityChanged,
    InvalidName,
    InvalidLength,
    Truncated,
    IoFailure,
    ResourceLimit,
    ClosedOrFailed,
    DependentsAlive,
    CollisionLimit,
    WrongState
};
enum class RootOperation {
    None,
    Open,
    OpenAt,
    Stat,
    NamedStat,
    Lock,
    Read,
    Close,
    CreateAt,
    Random128,
    WriteAt,
    SyncFile,
    UnlinkAt
};
enum class RootRole {
    Root,
    LockFile,
    Current,
    Base,
    Tail,
    Watermark,
    StagedBase,
    StagedTail,
    StagedWatermark
};
enum class RootStage { Before, Call, After };
enum class ArtifactRole { Base, Tail, Watermark };

// Scalar diagnostics own no path or file contents. errno is captured immediately.
struct RootError {
    RootErrorCode code = RootErrorCode::None;
    RootOperation operation = RootOperation::None;
    RootRole role = RootRole::Root;
    RootStage stage = RootStage::Call;
    int system_errno = 0;
    bool operator==(const RootError&) const = default;
};
struct RootIoEvent {
    RootOperation operation;
    RootRole role;
    RootStage stage;
    std::int64_t result; // Only meaningful After; no borrowed data survives the hook.
    int system_errno;
};
namespace detail {
struct RootState;
struct StagingAccess;
} // namespace detail
class RootLease;
class PinnedFile;

// Per-lease trusted syscall adapter, owned through shared_ptr. Default methods
// invoke POSIX. Overrides are noexcept and obey POSIX ownership/errno contracts.
// Adapters must not reenter owners, retain read buffers, or independently close
// borrowed descriptors. Only the close override consumes descriptor ownership.
// observe returns 0 to continue, or a positive errno to reject that boundary.
// After-open/create rejection still closes the acquired FD. Close is ALWAYS attempted
// once even if its Before hook rejects: a hook cannot suppress resource release.
// close must consume its ownership once, even on error. Real POSIX close errors
// can be ambiguous (notably EINTR); never retry/reopen using the old FD number.
// This cannot guarantee absence of kernel FD leaks on every ambiguous OS close.
class RootIo {
  public:
    virtual ~RootIo() = default;

  protected:
    friend struct detail::RootState;
    friend struct detail::StagingAccess;
    friend class RootLease;
    friend class PinnedFile;
    // Backend customization only, not a public arbitrary-path opening API.
    virtual int open(const char* path, int flags) noexcept;
    virtual int open_at(int directory, const char* name, int flags) noexcept;
    virtual int stat(int fd, struct stat* info) noexcept;
    virtual int named_stat(int directory, const char* name, struct stat* info, int flags) noexcept;
    virtual int lock(int fd, int flags) noexcept;
    virtual ssize_t read(int fd, void* bytes, std::size_t count, off_t offset) noexcept;
    virtual int close(int fd) noexcept;
    virtual int observe(const RootIoEvent&) noexcept;
    virtual int create_at(int directory, const char* name, int flags, mode_t mode) noexcept;
    virtual int random_128(std::array<std::byte, 16>&) noexcept;
    virtual ssize_t write_at(int fd, const void* bytes, std::size_t count, off_t offset) noexcept;
    virtual int sync_file(int fd) noexcept;
    virtual int unlink_at(int directory, const char* name) noexcept;
};

struct FileIdentity {
    dev_t device{};
    ino_t inode{};
    std::uint64_t byte_length = 0;
    bool operator==(const FileIdentity&) const = default;
};
// Observed read-only inode, NOT validated bytes, selection, EOF or durability.
// Owns its FD and a lease on the root/LOCK. No raw descriptor escape.
class PinnedFile final {
  public:
    PinnedFile() noexcept = default;
    ~PinnedFile() noexcept;
    PinnedFile(const PinnedFile&) = delete;
    PinnedFile& operator=(const PinnedFile&) = delete;
    PinnedFile(PinnedFile&&) noexcept;
    PinnedFile& operator=(PinnedFile&&) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] RootError error() const noexcept;
    // Borrowed observation, invalidated by move/close/failure/destruction. Copies
    // remain historical observations and confer no ongoing identity guarantee.
    [[nodiscard]] const FileIdentity* identity() const noexcept;
    [[nodiscard]] bool recheck() noexcept;
    // Caller-owned output; <=64 KiB per pread, no allocation. All failures latch
    // the shared lease. Output can be partially filled on error; discard it then.
    // Identity checks surround even an empty read; success does not prove EOF.
    [[nodiscard]] bool read_exact(std::uint64_t offset, std::span<std::byte> output) noexcept;
    // Closing the file leaves its lease reference until destruction/move-away.
    [[nodiscard]] bool close_checked() noexcept;

  private:
    friend class RootLease;
    void dispose() noexcept;
    std::shared_ptr<detail::RootState> state_;
    int fd_ = -1;
    RootRole role_ = RootRole::Current;
    std::array<char, 33> name_{};
    FileIdentity identity_;
};

// Existing trusted root and existing regular single-link LOCK only. Caller
// serializes ALL operations/destruction across the lease and dependent files;
// shared ownership provides lifetime, not thread safety. Ancestors are trusted;
// checks do not claim protection against hostile concurrent directory mutation.
// No create/write/fsync/rename/unlink, selection, decoder, Database or writer.
class RootLease final {
  public:
    RootLease() noexcept = default;
    ~RootLease() noexcept = default;
    RootLease(const RootLease&) = delete;
    RootLease& operator=(const RootLease&) = delete;
    RootLease(RootLease&&) noexcept = default;
    RootLease& operator=(RootLease&&) noexcept = default;
    [[nodiscard]] static RootLease open_existing(const std::filesystem::path& root,
                                                 std::shared_ptr<RootIo> io = {}) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] RootError error() const noexcept;
    [[nodiscard]] bool recheck() noexcept;
    [[nodiscard]] PinnedFile pin_current() noexcept;
    [[nodiscard]] PinnedFile pin_generation_file(ArtifactRole, GenerationId) noexcept;
    // Requires no dependent pins or staging owners. Violating this fails terminally but does not
    // release LOCK from underneath them. Otherwise close LOCK, then root, once.
    [[nodiscard]] bool close_checked() noexcept;
    // Last dependent destruction closes artifact, then LOCK, then root. Cleanup
    // never throws; destructor-time errors are observable only by surviving peers.

  private:
    friend struct detail::StagingAccess;
    PinnedFile pin(RootRole, const std::array<char, 33>&) noexcept;
    std::shared_ptr<detail::RootState> state_;
    RootError local_error_{RootErrorCode::ClosedOrFailed};
};

} // namespace redisdb::persistence::v2
