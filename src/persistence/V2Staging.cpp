#include "redisdb/persistence/V2Staging.hpp"
#include "V2RootAccess.hpp"

#include <utility>

namespace redisdb::persistence::v2 {
using detail::StagingAccess;
StagedFile::~StagedFile() noexcept { dispose(); }
void StagedFile::dispose() noexcept {
    if (valid())
        (void)discard_checked();
    (void)StagingAccess::close(*this);
}
void StagedFile::take(StagedFile&& other) noexcept {
    root_ = std::move(other.root_);
    fd_ = std::exchange(other.fd_, -1);
    role_ = other.role_;
    generation_ = other.generation_;
    name_ = other.name_;
    identity_ = other.identity_;
    state_ = std::exchange(other.state_, State::Closed);
    linked_ = std::exchange(other.linked_, false);
    local_error_ = other.local_error_;
}
StagedFile::StagedFile(StagedFile&& other) noexcept { take(std::move(other)); }
StagedFile& StagedFile::operator=(StagedFile&& other) noexcept {
    if (this != &other) {
        dispose();
        const auto previous = error();
        take(std::move(other));
        if (previous.code != RootErrorCode::None && previous.code != RootErrorCode::ClosedOrFailed)
            local_error_ = previous; // Failed target disposal cannot restore health.
    }
    return *this;
}
StagedFile StagedFile::create_data(RootLease& root, ArtifactRole role,
                                   GenerationId generation) noexcept {
    StagedFile file;
    StagingAccess::borrow(file, root);
    file.role_ = role;
    file.generation_ = generation;
    if (!StagingAccess::create(file))
        (void)StagingAccess::close(file); // Retain any ambiguously created name.
    return file;
}
bool StagedFile::valid() const noexcept {
    return fd_ >= 0 && state_ != State::Closed && local_error_.code == RootErrorCode::None &&
           StagingAccess::healthy(*this);
}
RootError StagedFile::error() const noexcept {
    if (local_error_.code != RootErrorCode::None)
        return local_error_;
    const auto shared = StagingAccess::error(*this);
    return shared.code != RootErrorCode::None ? shared
           : valid()                          ? RootError{}
                                              : RootError{RootErrorCode::ClosedOrFailed};
}
const FileIdentity* StagedFile::identity() const noexcept { return valid() ? &identity_ : nullptr; }
std::uint64_t StagedFile::byte_length() const noexcept { return identity_.byte_length; }
bool StagedFile::recheck() noexcept { return valid() && StagingAccess::check(*this); }
bool StagedFile::append(ByteView bytes) noexcept {
    if (!valid())
        return false;
    if (state_ != State::Building)
        return StagingAccess::fail(*this, RootErrorCode::WrongState, RootOperation::WriteAt);
    return StagingAccess::append(*this, bytes);
}
bool StagedFile::read_exact(std::uint64_t offset, std::span<std::byte> output) noexcept {
    return valid() && StagingAccess::read(*this, offset, output);
}
bool StagedFile::seal_file() noexcept {
    if (!recheck())
        return false;
    if (state_ == State::SealedFile)
        return true;
    if (!StagingAccess::sync(*this) || !recheck())
        return false;
    state_ = State::SealedFile;
    return true;
}
bool StagedFile::discard_checked() noexcept {
    if (!recheck()) {
        (void)StagingAccess::close(*this);
        return false;
    }
    const auto removed = StagingAccess::unlink(*this);
    const auto closed = StagingAccess::close(*this);
    return removed && closed;
}
bool StagedFile::close_checked() noexcept {
    const auto was_valid = valid();
    const auto closed = StagingAccess::close(*this);
    return was_valid && closed;
}
} // namespace redisdb::persistence::v2
