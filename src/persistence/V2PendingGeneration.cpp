#include "redisdb/persistence/V2PendingGeneration.hpp"
#include "V2PendingAccess.hpp"

#include <utility>

namespace redisdb::persistence::v2 {
using detail::PendingAccess;
namespace {
constexpr std::array<ArtifactRole, 3> Roles{ArtifactRole::Base, ArtifactRole::Tail,
                                            ArtifactRole::Watermark};
bool operational(RootError e) noexcept {
    return e.code != RootErrorCode::None && e.code != RootErrorCode::ClosedOrFailed;
}
} // namespace

PendingGeneration::~PendingGeneration() noexcept { dispose(); }
void PendingGeneration::dispose() noexcept {
    if (root_)
        (void)finish(state_ != State::OwnedEmpty);
}
void PendingGeneration::take(PendingGeneration&& other) noexcept {
    root_ = std::move(other.root_);
    owns_claim_ = std::exchange(other.owns_claim_, false);
    selected_ = std::exchange(other.selected_, std::nullopt);
    captured_ = std::exchange(other.captured_, std::nullopt);
    if (other.candidate_) {
        candidate_.emplace(std::move(*other.candidate_));
        other.candidate_.reset();
    }
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (other.files_[i]) {
            files_[i].emplace(std::move(*other.files_[i]));
            other.files_[i].reset();
        }
    }
    state_ = std::exchange(other.state_, State::Empty);
    local_error_ = std::exchange(other.local_error_, RootError{});
}
PendingGeneration::PendingGeneration(PendingGeneration&& other) noexcept { take(std::move(other)); }
PendingGeneration& PendingGeneration::operator=(PendingGeneration&& other) noexcept {
    if (this != &other) {
        dispose();
        if (!operational(error()))
            take(std::move(other));
    }
    return *this;
}
PendingGeneration PendingGeneration::begin_after(RootLease& root, GenerationId selected,
                                                 LogicalSequence captured,
                                                 std::uint32_t max_candidates) noexcept {
    PendingGeneration owner;
    owner.local_error_ = root.error(); // Existing failure wins; no filesystem operation.
    if (owner.local_error_.code != RootErrorCode::None) {
        owner.state_ = State::Failed;
        return owner;
    }
    if (max_candidates == 0 || max_candidates > 4096) {
        owner.local_error_ = {RootErrorCode::ResourceLimit};
        owner.state_ = State::Failed;
        return owner;
    }
    if (!PendingAccess::claim(owner, root)) {
        owner.state_ = State::Failed;
        return owner;
    }
    owner.state_ = State::Acquiring;
    owner.selected_ = selected;
    owner.captured_ = captured;
    owner.candidate_.emplace(
        GenerationCandidate::find_after(root, selected, captured, max_candidates));
    if (!owner.candidate_->valid() || !owner.candidate_->recheck_unused()) {
        (void)owner.finish(true);
        return owner;
    }
    for (std::size_t i = 0; i < Roles.size(); ++i) {
        owner.files_[i].emplace(
            StagedFile::create_data(root, Roles[i], *owner.candidate_->generation()));
        if (!owner.files_[i]->valid() || !owner.candidate_->recheck_unused()) {
            (void)owner.finish(true);
            return owner;
        }
        for (std::size_t j = 0; j <= i; ++j) {
            if (!owner.files_[j]->recheck()) {
                (void)owner.finish(true);
                return owner;
            }
        }
    }
    if (!owner.check_owned()) {
        (void)owner.finish(true);
        return owner;
    }
    owner.state_ = State::OwnedEmpty;
    return owner;
}
bool PendingGeneration::valid() const noexcept {
    if (state_ != State::OwnedEmpty || local_error_.code != RootErrorCode::None ||
        !PendingAccess::healthy(*this) || !candidate_ || !candidate_->valid())
        return false;
    for (const auto& file : files_)
        if (!file || !file->valid())
            return false;
    return true;
}
RootError PendingGeneration::error() const noexcept {
    if (local_error_.code != RootErrorCode::None)
        return local_error_;
    const auto shared = PendingAccess::error(*this);
    return shared.code != RootErrorCode::None ? shared
           : valid()                          ? RootError{}
                                              : RootError{RootErrorCode::ClosedOrFailed};
}
const GenerationId* PendingGeneration::generation() const noexcept {
    return valid() ? candidate_->generation() : nullptr;
}
const GenerationId* PendingGeneration::selected_generation() const noexcept {
    return valid() ? &*selected_ : nullptr;
}
const LogicalSequence* PendingGeneration::captured_sequence() const noexcept {
    return valid() ? &*captured_ : nullptr;
}
const FileIdentity* PendingGeneration::identity(ArtifactRole role) const noexcept {
    if (!valid())
        return nullptr;
    for (std::size_t i = 0; i < Roles.size(); ++i)
        if (role == Roles[i])
            return files_[i]->identity();
    return nullptr;
}
bool PendingGeneration::check_owned() noexcept {
    if (!candidate_ || !candidate_->recheck_unused())
        return false;
    for (auto& file : files_)
        if (!file || !file->recheck())
            return false;
    return candidate_->recheck_unused();
}
bool PendingGeneration::recheck() noexcept {
    if (state_ != State::OwnedEmpty)
        return false;
    if (valid() && check_owned())
        return true;
    (void)finish(true);
    return false;
}
bool PendingGeneration::finish(bool retain) noexcept {
    if (!root_)
        return false;
    bool success = state_ == State::OwnedEmpty && valid();
    RootError first;
    const auto remember = [&](RootError e) {
        if (first.code == RootErrorCode::None && operational(e))
            first = e;
    };
    remember(error());
    if (success && !retain && !check_owned()) {
        success = false;
        remember(error());
    }
    if (!success)
        retain = true;
    for (auto& file : files_) {
        if (!file)
            continue;
        const auto ok = !retain && PendingAccess::healthy(*this) ? file->discard_checked()
                                                                 : file->close_checked();
        remember(file->error());
        if (!ok) {
            success = false;
            retain = true;
        }
        // close_checked does not release its lease. Destroy the contained owner
        // BEFORE candidate/claim release, after consuming its descriptor once.
        file.reset();
    }
    candidate_.reset();
    remember(PendingAccess::error(*this));
    remember(PendingAccess::release(*this));
    selected_.reset();
    captured_.reset();
    if (first.code != RootErrorCode::None) {
        local_error_ = first;
        success = false;
    } else if (!success) {
        local_error_ = {RootErrorCode::ClosedOrFailed};
    }
    state_ = !success ? State::Failed : retain ? State::ClosedRetained : State::Discarded;
    return success;
}
bool PendingGeneration::discard_checked() noexcept {
    return state_ == State::OwnedEmpty && finish(false);
}
bool PendingGeneration::close_checked() noexcept {
    return state_ == State::OwnedEmpty && finish(true);
}
} // namespace redisdb::persistence::v2
