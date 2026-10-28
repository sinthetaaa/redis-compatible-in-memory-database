#pragma once

#include "redisdb/persistence/V2Root.hpp"

#include <optional>

namespace redisdb::persistence::v2 {

// Observes an unused successor triple; owns no artifact and reserves no generation.
// The caller supplies authoritative selected generation/synchronized sequence and
// serializes all operations/destruction. Separate observations on the SAME lease
// may return the same generation. The shared lease excludes cooperating writers,
// not hostile directory mutation. No initialization or publication authority.
class GenerationCandidate final {
  public:
    GenerationCandidate() noexcept = default;
    ~GenerationCandidate() noexcept;
    GenerationCandidate(const GenerationCandidate&) = delete;
    GenerationCandidate& operator=(const GenerationCandidate&) = delete;
    GenerationCandidate(GenerationCandidate&&) noexcept;
    GenerationCandidate& operator=(GenerationCandidate&&) noexcept;

    // Budget 1..4096; invalid budget is rejected locally before filesystem entry.
    // Entered exhaustion/I/O/identity failures latch the existing shared root.
    [[nodiscard]] static GenerationCandidate find_after(RootLease&, GenerationId selected,
                                                        LogicalSequence captured_sequence,
                                                        std::uint32_t max_candidates) noexcept;
    // Healthy attached observation, not a promise that names remain absent.
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] RootError error() const noexcept;
    // Borrowed typed observation, invalidated by move/failure/destruction; a copy
    // confers no ownership, durable reservation or publication guarantee.
    [[nodiscard]] const GenerationId* generation() const noexcept;
    [[nodiscard]] bool recheck_unused() noexcept;

  private:
    friend struct detail::GenerationAccess;
    std::shared_ptr<detail::RootState> root_;
    std::optional<GenerationId> generation_;
    RootError local_error_{};
    void dispose() noexcept;
    void take(GenerationCandidate&&) noexcept;
};

} // namespace redisdb::persistence::v2
