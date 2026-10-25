#pragma once

#include "redisdb/net/UniqueFd.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace redisdb::persistence {

enum class SyncPolicy { Always, EverySecond, No };
struct AofOptions {
    bool enabled = false;
    std::filesystem::path file = "appendonly.aof";
    SyncPolicy sync = SyncPolicy::Always;
};

class PersistenceError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Injectable syscall seam for deterministic short-write/EINTR/disk failure tests.
// Empty functions select the real POSIX syscall; no runtime failure flags.
struct AofIo {
    std::function<ssize_t(int, const void*, std::size_t)> write;
    std::function<int(int)> sync;
};

inline constexpr std::size_t MaxAofRecordBytes = 64 * 1024 * 1024;
[[nodiscard]] std::string encode_effects(const storage::CommittedEffects& effects);
[[nodiscard]] storage::CommittedEffects decode_effects(std::string_view bytes);
[[nodiscard]] std::uint32_t crc32c(std::string_view bytes) noexcept;

// Exclusive regular-file ownership. Existing malformed files are never repaired
// or truncated automatically. The parent directory must already exist.
class AofLog final {
  public:
    AofLog(const AofOptions& options, const storage::Clock& clock, AofIo io = {});
    AofLog(const AofLog&) = delete;
    AofLog& operator=(const AofLog&) = delete;
    void recover(storage::Database& database);
    void append(const storage::CommittedEffects& effects);
    void tick();
    void finish(); // explicit checked synchronization at clean shutdown
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }

  private:
    void write_all(std::string_view bytes, int fd = -1);
    void synchronize(int fd);
    void publish_watermark();
    void flush();
    void require_healthy() const;
    net::UniqueFd fd_;
    net::UniqueFd directory_;
    std::filesystem::path watermark_;
    const storage::Clock& clock_;
    SyncPolicy policy_;
    AofIo io_;
    storage::MonotonicTimePoint next_sync_;
    std::uint64_t sequence_ = 0;
    std::uint64_t durable_sequence_ = 0;
    std::uint64_t durable_offset_ = 20;
    std::uint32_t durable_crc_ = 0;
    std::uint64_t end_offset_ = 20;
    std::uint32_t last_crc_ = 0;
    bool pending_ = false;
    bool recovered_ = false;
    bool failed_ = false;
};

} // namespace redisdb::persistence
