#include "redisdb/persistence/V2PendingBaseWrite.hpp"
#include "V2PendingBaseWriteAccess.hpp"

#include <type_traits>
#include <utility>

namespace redisdb::persistence::v2 {
using detail::PendingBaseWriteAccess;
namespace {
bool operational(RootError e) noexcept {
    return e.code != RootErrorCode::None && e.code != RootErrorCode::ClosedOrFailed;
}
static_assert(std::is_nothrow_copy_constructible_v<EncodedBaseSummary>);
} // namespace

PendingBaseWriter::~PendingBaseWriter() noexcept { dispose(); }
void PendingBaseWriter::dispose() noexcept {
    if (root_)
        (void)finish(!healthy());
}
void PendingBaseWriter::take(PendingBaseWriter&& other) noexcept {
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
    if (other.cursor_) {
        cursor_.emplace(std::move(*other.cursor_));
        other.cursor_.reset();
    }
    generated_ = std::exchange(other.generated_, std::nullopt);
    state_ = std::exchange(other.state_, State::Empty);
    failure_ = std::exchange(other.failure_, PendingBaseWriteFailure{});
}
PendingBaseWriter::PendingBaseWriter(PendingBaseWriter&& other) noexcept { take(std::move(other)); }
PendingBaseWriter& PendingBaseWriter::operator=(PendingBaseWriter&& other) noexcept {
    if (this != &other) {
        dispose();
        const auto previous = error();
        if (previous.code == PendingBaseWriteError::None ||
            previous.code == PendingBaseWriteError::Closed)
            take(std::move(other));
    }
    return *this;
}
PendingBaseWriter PendingBaseWriter::begin(PendingGeneration&& source, BaseEncodingContext context,
                                           BaseInputView input, BaseEncodeLimits limits) noexcept {
    PendingBaseWriter owner;
    const auto source_error = source.error();
    if (operational(source_error)) {
        owner.failure_ = {PendingBaseWriteError::RootFailure, {}, source_error};
        owner.state_ = State::Failed;
        return owner;
    }
    if (!source.valid()) {
        owner.failure_ = {PendingBaseWriteError::WrongState, {}, source_error};
        owner.state_ = State::Failed;
        return owner;
    }
    if (*source.selected_generation() != context.selected_generation ||
        *source.generation() != context.candidate_generation ||
        *source.captured_sequence() != context.captured_sequence) {
        owner.failure_.code = PendingBaseWriteError::ContextMismatch;
        owner.state_ = State::Failed;
        return owner;
    }
    auto cursor = BaseEncodingCursor::begin(context, input, limits);
    if (cursor.error().code != BaseEncodeError::None) {
        owner.failure_ = {PendingBaseWriteError::Encoding, cursor.error(), {}};
        owner.state_ = State::Failed;
        return owner;
    }
    // Cursor allocation/preflight is complete. Moving its buffers and the same
    // aggregate's owners is allocation-free and performs no filesystem call.
    if (!PendingBaseWriteAccess::transfer(owner, source)) {
        owner.failure_ = {PendingBaseWriteError::WrongState, {}, source.error()};
        owner.state_ = State::Failed;
        return owner;
    }
    owner.cursor_.emplace(std::move(cursor));
    return owner;
}
bool PendingBaseWriter::healthy() const noexcept {
    return (state_ == State::Writing || state_ == State::WrittenUnsealed) &&
           failure_.code == PendingBaseWriteError::None && PendingBaseWriteAccess::healthy(*this);
}
PendingBaseWriteFailure PendingBaseWriter::error() const noexcept {
    const auto shared = PendingBaseWriteAccess::error(*this);
    if (operational(shared)) {
        auto failure = failure_;
        if (failure.code == PendingBaseWriteError::None)
            failure.code = PendingBaseWriteError::RootFailure;
        failure.root = shared;
        return failure;
    }
    if (failure_.code != PendingBaseWriteError::None)
        return failure_;
    return healthy() ? PendingBaseWriteFailure{}
                     : PendingBaseWriteFailure{PendingBaseWriteError::Closed, {}, {}};
}
bool PendingBaseWriter::written_unsealed() const noexcept {
    return state_ == State::WrittenUnsealed && healthy() && generated_.has_value();
}
const EncodedBaseSummary* PendingBaseWriter::generated() const noexcept {
    return written_unsealed() ? &*generated_ : nullptr;
}
PendingBaseWriteStep PendingBaseWriter::fail(PendingBaseWriteError code, RootErrorCode root_code,
                                             BaseEncodeFailure encoding) noexcept {
    const auto shared = PendingBaseWriteAccess::error(*this);
    if (failure_.code == PendingBaseWriteError::None)
        failure_ = {operational(shared) ? PendingBaseWriteError::RootFailure : code, encoding,
                    shared};
    PendingBaseWriteAccess::fail(*this, root_code); // First shared error wins.
    state_ = State::Failed;
    generated_.reset();
    cursor_.reset();
    (void)finish(true);
    return PendingBaseWriteStep::Error;
}
PendingBaseWriteStep PendingBaseWriter::advance() noexcept {
    if (state_ != State::Writing && state_ != State::WrittenUnsealed)
        return PendingBaseWriteStep::Error;
    if (!healthy() || !PendingBaseWriteAccess::check(*this))
        return fail(PendingBaseWriteError::RootFailure, RootErrorCode::WrongState);
    if (state_ == State::WrittenUnsealed)
        return PendingBaseWriteStep::WrittenUnsealed;
    if (!cursor_)
        return fail(PendingBaseWriteError::Encoding, RootErrorCode::WrongState);
    const auto step = cursor_->next();
    if (step == BaseEncodeStep::Error) {
        const auto e = cursor_->error();
        const auto root_code =
            e.code == BaseEncodeError::ResourceLimit ? RootErrorCode::ResourceLimit
            : e.code == BaseEncodeError::ExtentOverflow || e.code == BaseEncodeError::ChargeOverflow
                ? RootErrorCode::InvalidLength
                : RootErrorCode::WrongState;
        return fail(PendingBaseWriteError::Encoding, root_code, e);
    }
    if (step == BaseEncodeStep::Chunk) {
        // Never cache a span across cursor moves/next(). A partially accepted
        // chunk is terminal on append failure and is never replayed.
        if (!files_[0]->append(cursor_->bytes()) || !PendingBaseWriteAccess::check(*this))
            return fail(PendingBaseWriteError::RootFailure, RootErrorCode::WrongState);
        return PendingBaseWriteStep::More;
    }
    const auto* summary = cursor_->result();
    if (!summary || summary->identity().byte_length != cursor_->bytes_emitted() ||
        summary->identity().byte_length != files_[0]->byte_length() ||
        summary->identity().generation != *candidate_->generation() ||
        summary->identity().sequence != *captured_)
        return fail(PendingBaseWriteError::ExtentMismatch, RootErrorCode::InvalidLength);
    if (!PendingBaseWriteAccess::check(*this))
        return fail(PendingBaseWriteError::RootFailure, RootErrorCode::WrongState);
    generated_.emplace(*summary);
    cursor_.reset();
    state_ = State::WrittenUnsealed;
    return PendingBaseWriteStep::WrittenUnsealed;
}
bool PendingBaseWriter::recheck() noexcept {
    if (state_ != State::Writing && state_ != State::WrittenUnsealed)
        return false;
    if (healthy() && PendingBaseWriteAccess::check(*this))
        return true;
    (void)fail(PendingBaseWriteError::RootFailure, RootErrorCode::WrongState);
    return false;
}
bool PendingBaseWriter::finish(bool retain) noexcept {
    if (!root_)
        return false;
    bool success = healthy();
    RootError first = error().root;
    if (!operational(first))
        first = {};
    const auto remember = [&](RootError e) noexcept {
        if (first.code == RootErrorCode::None && operational(e))
            first = e;
    };
    generated_.reset();
    cursor_.reset();
    if (success && !retain && !PendingBaseWriteAccess::check(*this)) {
        success = false;
        remember(PendingBaseWriteAccess::error(*this));
    }
    if (!success)
        retain = true;
    for (auto& file : files_) {
        if (!file)
            continue;
        const auto ok = !retain && PendingBaseWriteAccess::healthy(*this) ? file->discard_checked()
                                                                          : file->close_checked();
        remember(file->error());
        if (!ok) {
            success = false;
            retain = true;
        }
        file.reset(); // Artifact descriptors consumed before final LOCK/root.
    }
    candidate_.reset();
    remember(PendingBaseWriteAccess::error(*this));
    remember(PendingBaseWriteAccess::release(*this));
    selected_.reset();
    captured_.reset();
    if (first.code != RootErrorCode::None) {
        failure_.root = first;
        if (failure_.code == PendingBaseWriteError::None)
            failure_.code = PendingBaseWriteError::RootFailure;
        success = false;
    }
    state_ = !success ? State::Failed : retain ? State::ClosedRetained : State::Discarded;
    return success;
}
bool PendingBaseWriter::discard_checked() noexcept {
    return (state_ == State::Writing || state_ == State::WrittenUnsealed) && finish(false);
}
bool PendingBaseWriter::close_checked() noexcept {
    return (state_ == State::Writing || state_ == State::WrittenUnsealed) && finish(true);
}
} // namespace redisdb::persistence::v2
