#pragma once

#include "redisdb/persistence/V2Root.hpp"

namespace redisdb::persistence::v2 {

// Owns only an unpublished, unselected data temporary. Callers serialize access.
// Sealing checks file fsync only: no content, EOF, directory durability, recovery
// unit, selection, publication or writer-activation claim follows from success.
class StagedFile final {
  public:
    StagedFile() noexcept = default;
    ~StagedFile() noexcept;
    StagedFile(const StagedFile&) = delete;
    StagedFile& operator=(const StagedFile&) = delete;
    StagedFile(StagedFile&&) noexcept;
    StagedFile& operator=(StagedFile&&) noexcept;
    [[nodiscard]] static StagedFile create_data(RootLease&, ArtifactRole, GenerationId) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] RootError error() const noexcept;
    // Borrowed observation, invalidated by move, close, failure or destruction.
    [[nodiscard]] const FileIdentity* identity() const noexcept;
    // Historical accepted write extent, including progress before terminal failure.
    [[nodiscard]] std::uint64_t byte_length() const noexcept;
    [[nodiscard]] bool recheck() noexcept;
    [[nodiscard]] bool append(ByteView) noexcept;
    // Failure may partially fill output; discard that output on failure.
    [[nodiscard]] bool read_exact(std::uint64_t, std::span<std::byte>) noexcept;
    // Repeated success revalidates all identities/extent; it does not resync.
    [[nodiscard]] bool seal_file() noexcept;
    [[nodiscard]] bool discard_checked() noexcept;
    // Consumes descriptor once, retaining the linked temporary and root lifetime.
    [[nodiscard]] bool close_checked() noexcept;

  private:
    friend struct detail::StagingAccess;
    enum class State { Building, SealedFile, Closed };
    std::shared_ptr<detail::RootState> root_;
    int fd_ = -1;
    ArtifactRole role_ = ArtifactRole::Base;
    GenerationId generation_{std::uint64_t{1}};
    std::array<char, 70> name_{};
    FileIdentity identity_{};
    State state_ = State::Closed;
    bool linked_ = false;
    RootError local_error_{};
    void dispose() noexcept;
    void take(StagedFile&&) noexcept;
};

} // namespace redisdb::persistence::v2
