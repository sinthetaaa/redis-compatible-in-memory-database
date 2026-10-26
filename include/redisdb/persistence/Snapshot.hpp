#pragma once

#include "redisdb/persistence/AofLog.hpp"

namespace redisdb::persistence {

struct SnapshotOptions {
    // Existing dedicated directory; empty disables snapshots. Requires AOF.
    std::filesystem::path directory;
};

enum class SnapshotBoundary {
    DataWritten,
    DataSynced,
    DataRenamed,
    DataDirectorySynced,
    ManifestWritten,
    ManifestSynced,
    ManifestRenamed,
    Published
};

// Test-only injected syscalls and observation points; no environment failure knobs.
struct SnapshotIo {
    AofIo file;
    std::function<int(const char*, const char*)> rename;
    std::function<void(SnapshotBoundary)> boundary;
};

// Serialized with command execution and journal ownership. Keeps every immutable
// generation, including unpublished candidates. Only CURRENT selects recovery.
class SnapshotStore final {
  public:
    explicit SnapshotStore(const SnapshotOptions& options, SnapshotIo io = {});
    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;
    [[nodiscard]] std::optional<AofCheckpoint> restore(storage::Database& database);
    [[nodiscard]] std::uint64_t publish(const storage::Database& database, AofLog& journal);
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

  private:
    void write_all(int fd, std::string_view bytes);
    void sync(int fd);
    void rename(const std::filesystem::path& from, const std::filesystem::path& to);
    void boundary(SnapshotBoundary point);
    void select(std::uint64_t generation);
    [[nodiscard]] std::filesystem::path data_path(std::uint64_t generation) const;
    std::filesystem::path path_;
    net::UniqueFd directory_;
    net::UniqueFd lock_;
    SnapshotIo io_;
    std::uint64_t generation_ = 0;
    bool restored_ = false;
    bool failed_ = false;
};

} // namespace redisdb::persistence
