#pragma once

#include "redisdb/persistence/V2Generation.hpp"
#include "redisdb/persistence/V2Staging.hpp"

#include <array>
#include <memory>
#include <optional>

namespace redisdb::persistence::v2 {
namespace detail {
struct PendingAccess;
struct PendingBaseWriteAccess;
}

// One new-factory admission and three EMPTY, unpublished data temporaries.
// Caller-attested successor context; no CURRENT authentication, initialization,
// content/seal/install capability, durable reservation or publication authority.
// Callers serialize all operations/destruction, including other lease dependents.
// Existing raw candidate/staging APIs do not participate in this admission slot.
class PendingGeneration final {
  public:
    PendingGeneration() noexcept = default;
    ~PendingGeneration() noexcept;
    PendingGeneration(const PendingGeneration&) = delete;
    PendingGeneration& operator=(const PendingGeneration&) = delete;
    PendingGeneration(PendingGeneration&&) noexcept;
    // A failed target/disposal preserves its first error AND leaves source intact.
    // Otherwise consumes source. Self-move is a no-op; transfer itself does no I/O.
    PendingGeneration& operator=(PendingGeneration&&) noexcept;
    [[nodiscard]] static PendingGeneration begin_after(RootLease&, GenerationId selected,
                                                       LogicalSequence captured,
                                                       std::uint32_t max_candidates) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] RootError error() const noexcept;
    // Borrowed observations only while healthy OwnedEmpty; invalidated by move,
    // failure, terminal operation or destruction. No fresh-absence guarantee.
    [[nodiscard]] const GenerationId* generation() const noexcept;
    [[nodiscard]] const GenerationId* selected_generation() const noexcept;
    [[nodiscard]] const LogicalSequence* captured_sequence() const noexcept;
    // Invalid role returns nullptr locally. No mutable child/name/FD escapes.
    [[nodiscard]] const FileIdentity* identity(ArtifactRole) const noexcept;
    [[nodiscard]] bool recheck() noexcept;
    [[nodiscard]] bool discard_checked() noexcept;
    // Retains names, drains all contained owners and releases admission/lease.
    [[nodiscard]] bool close_checked() noexcept;

  private:
    friend struct detail::PendingAccess;
    friend struct detail::PendingBaseWriteAccess;
    enum class State { Empty, Acquiring, OwnedEmpty, Discarded, ClosedRetained, Failed };
    std::shared_ptr<detail::RootState> root_;
    bool owns_claim_ = false;
    std::optional<GenerationId> selected_;
    std::optional<LogicalSequence> captured_;
    std::optional<GenerationCandidate> candidate_;
    std::array<std::optional<StagedFile>, 3> files_; // Base, Tail, Watermark only.
    State state_ = State::Empty;
    RootError local_error_{};
    void dispose() noexcept;
    void take(PendingGeneration&&) noexcept;
    bool check_owned() noexcept;
    bool finish(bool retain) noexcept;
};
} // namespace redisdb::persistence::v2
