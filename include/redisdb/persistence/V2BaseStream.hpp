#pragma once

#include "redisdb/persistence/V2Format.hpp"
#include "redisdb/storage/Database.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redisdb::persistence::v2 {

inline constexpr std::size_t MaxKeyPayload = 67'108'864;
inline constexpr std::size_t MaxLruPayload = 8'388'608;

enum class StreamStatus { NeedMore, Complete, Error };
enum class BaseStreamError {
    None,
    Header,
    Identity,
    Length,
    Frame,
    Checksum,
    Trailer,
    Semantic,
    Order,
    Permutation,
    Charge,
    Truncated,
    TrailingBytes,
    Finalized,
    ResourceLimit
};

class BaseFrameError final : public std::runtime_error {
  public:
    explicit BaseFrameError(BaseStreamError code)
        : std::runtime_error("invalid V2 base frame"), code_(code) {}
    [[nodiscard]] BaseStreamError code() const noexcept { return code_; }

  private:
    BaseStreamError code_;
};

// Optional caller budgets; charge is model-1 policy units, not an allocation/RSS
// bound. Allocator/container failures are also surfaced as ResourceLimit.
struct BaseStreamLimits {
    std::size_t frame_payload = MaxKeyPayload;
    std::size_t entries = std::numeric_limits<std::size_t>::max();
    std::size_t logical_charge = std::numeric_limits<std::size_t>::max();
};

class ValidatedBase final {
  public:
    [[nodiscard]] const BaseIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] const BaseHeaderFields& header() const noexcept { return header_; }
    [[nodiscard]] std::span<const storage::EntryEffect> entries() const noexcept {
        return entries_;
    }
    // Indices into entries(), oldest to newest. Empty when no history was stored.
    // In that case entries() itself supplies unsigned-byte sorted cold-start order.
    [[nodiscard]] std::span<const std::size_t> lru_order() const noexcept { return lru_; }
    [[nodiscard]] Crc32c prefix_crc() const noexcept { return Crc32c{prefix_crc_}; }
    [[nodiscard]] Crc32c done_crc() const noexcept { return Crc32c{done_crc_}; }

  private:
    friend class BaseStreamValidator;
    ValidatedBase(BaseIdentity identity, BaseHeaderFields header,
                  std::vector<storage::EntryEffect> entries, std::vector<std::size_t> lru,
                  std::uint32_t prefix, std::uint32_t done);
    BaseIdentity identity_;
    BaseHeaderFields header_;
    std::vector<storage::EntryEffect> entries_;
    std::vector<std::size_t> lru_;
    std::uint32_t prefix_crc_, done_crc_;
};

// Pure serialized-content validation, never selection or proof of fsync/file
// identity. Caller serializes calls, supplies immutable untrusted claims including
// the exact input extent, feeds every byte in order, and calls finish() only once
// actual EOF at that pinned extent is established. A DONE alone is NeedMore.
// Spans are borrowed only during feed(); every retained byte/value is owned.
// Memory: one frame <= MaxKeyPayload+16, plus O(K + decoded values) state and
// O(K) permutation index. Counts never cause aggregate preallocation. Budgets do
// not promise constant total memory. No callbacks, clocks, live DB or I/O.
class BaseStreamValidator final {
  public:
    explicit BaseStreamValidator(BaseReferenceFields expected, BaseStreamLimits limits = {});
    explicit BaseStreamValidator(const BaseIdentity& expected, BaseStreamLimits limits = {});
    BaseStreamValidator(const BaseStreamValidator&) = delete;
    BaseStreamValidator& operator=(const BaseStreamValidator&) = delete;
    [[nodiscard]] StreamStatus feed(ByteView bytes) noexcept;
    // Repeated finish after success is idempotent; any feed after success fails
    // (even empty), invalidating result(). Every error is terminal and stable.
    [[nodiscard]] StreamStatus finish() noexcept;
    [[nodiscard]] StreamStatus status() const noexcept { return status_; }
    [[nodiscard]] BaseStreamError error() const noexcept { return error_; }
    // Available only after Complete. Borrowed until destruction or a failed feed.
    [[nodiscard]] const ValidatedBase* result() const noexcept;
    [[nodiscard]] std::uint64_t bytes_received() const noexcept { return received_; }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffer_.size(); }

  private:
    struct UnsignedLess {
        bool operator()(const std::string& a, const std::string& b) const noexcept;
    };
    StreamStatus fail(BaseStreamError) noexcept;
    void accept_header();
    void accept_frame_header();
    void accept_frame();
    BaseReferenceFields expected_;
    BaseStreamLimits limits_;
    StreamStatus status_ = StreamStatus::NeedMore;
    BaseStreamError error_ = BaseStreamError::None;
    std::string buffer_;
    std::size_t target_ = BaseHeaderBytes;
    std::optional<BaseHeaderFields> header_;
    std::vector<storage::EntryEffect> entries_;
    std::vector<std::size_t> lru_;
    std::map<std::string, std::size_t, UnsignedLess> remaining_;
    std::optional<ValidatedBase> result_;
    std::uint64_t received_ = 0, keys_ = 0, lrus_ = 0;
    std::size_t charge_ = 0;
    std::uint32_t file_crc_ = 0, prefix_crc_ = 0, done_crc_ = 0;
    bool frame_header_ = true, done_ = false;
};

// Owned frame bytes only; caller maintains the pre-DONE CRC and total extent.
// V1 payload limits/representation, including deadline flooring, are unchanged.
// Invalid framing throws BaseFrameError; inherited payload rejection throws
// PersistenceError. Allocation failures propagate; no partial frame is returned.
[[nodiscard]] std::string encode_key_frame(const storage::EntryEffect& replacement);
[[nodiscard]] std::string encode_lru_frame(std::string_view binary_key);
[[nodiscard]] std::string encode_done_frame(Crc32c prefix);

} // namespace redisdb::persistence::v2
