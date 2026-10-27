#pragma once

#include "redisdb/persistence/V2BaseStream.hpp" // Existing StreamStatus and fixed metadata types.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace redisdb::persistence::v2 {

inline constexpr std::size_t MaxTailPayload = 67'108'864;
inline constexpr std::size_t MaxTailRecordBytes = MaxTailPayload + 24;

enum class TailStreamError {
    None,
    Length,
    Header,
    Identity,
    Marker,
    Frame,
    Sequence,
    Checksum,
    Trailer,
    Semantic,
    Boundary,
    Truncated,
    Finalized,
    ResourceLimit
};

// Budgets restrict payload bytes per record and the total number of records.
// They are not an RSS bound: one temporary V1 decoded effects object can require
// additional per-record memory. No historical transactions are retained.
struct TailStreamLimits {
    std::size_t record_payload = MaxTailPayload;
    std::uint64_t records = UINT64_MAX;
};

class ValidatedTailScan final {
  public:
    [[nodiscard]] const TailHeaderIdentity& identity() const noexcept { return observed_.tail; }
    [[nodiscard]] const TailHeaderFields& header() const noexcept { return header_; }
    [[nodiscard]] const GenerationCheckpoint& observed() const noexcept { return observed_; }
    // An encoded watermark claim matched to actual bytes, NOT proof of fsync.
    [[nodiscard]] const GenerationCheckpoint& matched_boundary() const noexcept { return matched_; }
    [[nodiscard]] std::uint64_t byte_length() const noexcept { return observed_.offset.value(); }
    [[nodiscard]] std::uint64_t record_count() const noexcept { return records_; }

  private:
    friend class TailStreamValidator;
    ValidatedTailScan(TailHeaderFields, GenerationCheckpoint observed, GenerationCheckpoint matched,
                      std::uint64_t records) noexcept;
    TailHeaderFields header_;
    GenerationCheckpoint observed_, matched_;
    std::uint64_t records_;
};

// Pure serialized-tail validation. Caller serializes calls, supplies owned copies
// of untrusted immutable header/marker claims and the exact declared extent, feeds
// ALL bytes in order, then calls finish() only at caller-established actual EOF.
// Extent must be 96..MaxFileBytes (the supported macOS/Linux signed-64 offset
// ceiling). Matching a watermark never stops suffix validation or exposes a result.
// This does not verify the base file, file identities, locks, CURRENT, fsync,
// durability, replay correctness against a keyspace, or permission to serve.
// No I/O, callbacks, clocks, replay or live Database mutation.
// Spans are borrowed only during feed(). One owned buffer (<=MaxTailRecordBytes)
// plus constant metadata and temporary decoded effects; no whole-tail buffering,
// historical vector, aggregate reserve, or recency/permutation reconstruction.
class TailStreamValidator final {
  public:
    TailStreamValidator(TailHeaderFields expected, MarkerFields marker,
                        std::uint64_t declared_extent, TailStreamLimits limits = {});
    TailStreamValidator(const TailStreamValidator&) = delete;
    TailStreamValidator& operator=(const TailStreamValidator&) = delete;
    [[nodiscard]] StreamStatus feed(ByteView bytes) noexcept;
    [[nodiscard]] StreamStatus finish() noexcept;
    [[nodiscard]] StreamStatus status() const noexcept { return status_; }
    [[nodiscard]] TailStreamError error() const noexcept { return error_; }
    // Only after successful finish. Borrowed until destruction or a rejected feed.
    [[nodiscard]] const ValidatedTailScan* result() const noexcept;
    [[nodiscard]] std::uint64_t bytes_received() const noexcept { return received_; }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return used_; }
    // Requested character-array storage, excluding allocator overhead/decoded effects.
    [[nodiscard]] std::size_t buffer_capacity() const noexcept { return capacity_; }
    // Repeated finish is idempotent. ANY feed after success, including empty,
    // invalidates result() and fails. Every failure is terminal and deterministic.

  private:
    StreamStatus fail(TailStreamError) noexcept;
    void grow(std::size_t);
    void accept_header();
    void accept_record_header();
    void accept_record();
    void match_boundary();
    TailHeaderFields expected_;
    MarkerFields marker_;
    std::uint64_t extent_;
    TailStreamLimits limits_;
    StreamStatus status_ = StreamStatus::NeedMore;
    TailStreamError error_ = TailStreamError::None;
    std::unique_ptr<char[]> buffer_;
    std::size_t capacity_ = 0, used_ = 0, target_ = TailHeaderBytes;
    std::optional<TailHeaderIdentity> identity_;
    std::optional<ValidatedTailScan> result_;
    SourceTuple observed_;
    std::uint64_t received_ = 0, records_ = 0;
    bool header_accepted_ = false, record_header_ = true, matched_ = false;
};

} // namespace redisdb::persistence::v2
