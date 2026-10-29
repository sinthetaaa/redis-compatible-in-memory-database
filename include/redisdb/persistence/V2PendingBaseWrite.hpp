#pragma once

#include "redisdb/persistence/V2BaseEncoding.hpp"
#include "redisdb/persistence/V2PendingGeneration.hpp"

namespace redisdb::persistence::v2 {
namespace detail {
struct PendingBaseWriteAccess;
}

enum class PendingBaseWriteStep { More, WrittenUnsealed, Error };
enum class PendingBaseWriteError {
    None,
    Closed,
    WrongState,
    ContextMismatch,
    Encoding,
    RootFailure,
    ExtentMismatch
};
struct PendingBaseWriteFailure {
    PendingBaseWriteError code = PendingBaseWriteError::None;
    BaseEncodeFailure encoding{};
    RootError root{};
};

// Consumes one empty admitted aggregate, never a second claim/root/LOCK.
// Serialized access and caller-frozen input lifetimes are required, including
// across moves. Completion releases cursor borrows, but grants only generated
// bytes + checked write extent: no seal/fsync/readback/fidelity or selection.
class PendingBaseWriter final {
  public:
    PendingBaseWriter() noexcept = default;
    ~PendingBaseWriter() noexcept;
    PendingBaseWriter(const PendingBaseWriter&) = delete;
    PendingBaseWriter& operator=(const PendingBaseWriter&) = delete;
    PendingBaseWriter(PendingBaseWriter&&) noexcept;
    // Failed target disposal leaves the incoming independent owner untouched.
    PendingBaseWriter& operator=(PendingBaseWriter&&) noexcept;
    // Pure rejection before transfer preserves source. This does not authorize
    // continued serving/retry by a future entered publication coordinator.
    static PendingBaseWriter begin(PendingGeneration&&, BaseEncodingContext, BaseInputView,
                                   BaseEncodeLimits) noexcept;
    PendingBaseWriteStep advance() noexcept;
    bool written_unsealed() const noexcept;
    PendingBaseWriteFailure error() const noexcept;
    // Borrowed scalar metadata only while healthy WrittenUnsealed; invalidated
    // by failure, move, disposal or destruction. Not independently checked disk.
    const EncodedBaseSummary* generated() const noexcept;
    bool recheck() noexcept;
    bool discard_checked() noexcept;
    bool close_checked() noexcept;

  private:
    friend struct detail::PendingBaseWriteAccess;
    enum class State { Empty, Writing, WrittenUnsealed, Discarded, ClosedRetained, Failed };
    std::shared_ptr<detail::RootState> root_;
    bool owns_claim_ = false;
    std::optional<GenerationId> selected_;
    std::optional<LogicalSequence> captured_;
    std::optional<GenerationCandidate> candidate_;
    std::array<std::optional<StagedFile>, 3> files_;
    std::optional<BaseEncodingCursor> cursor_;
    std::optional<EncodedBaseSummary> generated_;
    State state_ = State::Empty;
    PendingBaseWriteFailure failure_{};
    bool healthy() const noexcept;
    void take(PendingBaseWriter&&) noexcept;
    void dispose() noexcept;
    bool finish(bool retain) noexcept;
    PendingBaseWriteStep fail(PendingBaseWriteError, RootErrorCode,
                              BaseEncodeFailure = {}) noexcept;
};
} // namespace redisdb::persistence::v2
