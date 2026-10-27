#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace redisdb::persistence::v2 {

inline constexpr std::size_t BaseHeaderBytes = 184;
inline constexpr std::size_t TailHeaderBytes = 96;
inline constexpr std::size_t MarkerBytes = 112;
inline constexpr std::size_t SelectorBytes = 112;
inline constexpr std::uint64_t MaxFileBytes = INT64_MAX;
using ByteView = std::span<const std::byte>;
using StoreBytes = std::array<std::byte, 16>;

enum class Error {
    InvalidLength,
    InvalidMagic,
    UnsupportedVersion,
    UnsupportedFlags,
    InvalidAccounting,
    InvalidReserved,
    InvalidChecksum,
    InvalidTrailer,
    InvalidIdentity,
    InvalidProvenance,
    InvalidCounts,
    InvalidBoundary,
    CrossHeaderMismatch,
    GenerationExhausted,
    SequenceExhausted
};
class FormatError final : public std::runtime_error {
  public:
    explicit FormatError(Error code);
    [[nodiscard]] Error code() const noexcept { return code_; }

  private:
    Error code_;
};

class StoreId final {
  public:
    explicit StoreId(StoreBytes bytes);
    [[nodiscard]] const StoreBytes& bytes() const noexcept { return bytes_; }
    bool operator==(const StoreId&) const = default;

  private:
    StoreBytes bytes_;
};

// Tags keep integer domains distinct; no default construction or implicit narrowing.
namespace detail {
template <class Tag, class T, T Min, T Max> class Number final {
  public:
    explicit constexpr Number(T value) : value_(value) {
        if (value < Min || value > Max)
            throw FormatError(Error::InvalidIdentity);
    }
    template <class U>
        requires(!std::same_as<U, T>)
    Number(U) = delete;
    [[nodiscard]] constexpr T value() const noexcept { return value_; }
    bool operator==(const Number&) const = default;

  private:
    T value_;
};
struct GenerationTag;
struct SequenceTag;
struct OffsetTag;
struct CrcTag;
} // namespace detail
using GenerationId = detail::Number<detail::GenerationTag, std::uint64_t, 1, UINT64_MAX>;
using LogicalSequence = detail::Number<detail::SequenceTag, std::uint64_t, 0, UINT64_MAX>;
using TailOffset = detail::Number<detail::OffsetTag, std::uint64_t, 96, MaxFileBytes>;
using Crc32c = detail::Number<detail::CrcTag, std::uint32_t, 0, UINT32_MAX>;

[[nodiscard]] GenerationId next_generation(GenerationId);
[[nodiscard]] LogicalSequence next_sequence(LogicalSequence);
[[nodiscard]] GenerationId parse_generation(std::string_view);
[[nodiscard]] std::string format_generation(GenerationId);
[[nodiscard]] Crc32c checksum(ByteView) noexcept;

// These value identities carry claims, not proof of full-file validation/fsync.
struct BaseIdentity {
    StoreId store;
    GenerationId generation;
    LogicalSequence sequence;
    std::uint64_t byte_length;
    Crc32c header_crc;
    Crc32c file_crc;
    bool operator==(const BaseIdentity&) const = default;
};
struct TailHeaderIdentity {
    BaseIdentity base;
    std::uint32_t header_length;
    Crc32c header_crc;
    Crc32c full_header_crc;
    bool operator==(const TailHeaderIdentity&) const = default;
};
struct GenerationCheckpoint {
    TailHeaderIdentity tail;
    LogicalSequence sequence;
    TailOffset offset;
    Crc32c record_crc;
    Crc32c prefix_crc;
    bool operator==(const GenerationCheckpoint&) const = default;
};

// Owned, deliberately untrusted wire-field values. Encoders revalidate them.
// No views into caller buffers survive decoding. Checksums are always derived.
struct SourceTuple {
    std::uint64_t sequence = 0;
    std::uint64_t offset = 0;
    std::uint32_t record_crc = 0;
    std::uint32_t prefix_crc = 0;
    bool operator==(const SourceTuple&) const = default;
};
struct ProvenanceFields {
    std::uint32_t kind = 0;
    StoreBytes store{};
    std::uint64_t generation = 0;
    SourceTuple observed;
    SourceTuple durable;
    std::uint64_t snapshot_generation = 0;
    bool operator==(const ProvenanceFields&) const = default;
};
struct BaseHeaderFields {
    std::uint32_t flags = 0;
    std::uint32_t accounting_model = 1;
    StoreBytes store{};
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint64_t key_count = 0;
    std::uint64_t logical_charge = 0;
    std::uint64_t lru_count = 0;
    ProvenanceFields source;
    bool operator==(const BaseHeaderFields&) const = default;
};
struct BaseReferenceFields {
    std::uint32_t accounting_model = 1;
    StoreBytes store{};
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint64_t byte_length = 0;
    std::uint32_t file_crc = 0;
    std::uint32_t header_crc = 0;
    bool operator==(const BaseReferenceFields&) const = default;
};
struct TailHeaderFields {
    BaseReferenceFields base;
    bool operator==(const TailHeaderFields&) const = default;
};
struct TailReferenceFields {
    StoreBytes store{};
    std::uint64_t generation = 0;
    std::uint32_t header_length = 96;
    std::uint32_t header_crc = 0;
    std::uint32_t full_header_crc = 0;
    bool operator==(const TailReferenceFields&) const = default;
};
struct MarkerFields {
    TailReferenceFields tail;
    SourceTuple durable; // Generation-local coordinates; not V1 AofCheckpoint.
    bool operator==(const MarkerFields&) const = default;
};
struct SelectorFields {
    BaseReferenceFields base;
    std::uint32_t tail_header_length = 96;
    std::uint32_t tail_header_crc = 0;
    std::uint32_t tail_full_header_crc = 0;
    bool operator==(const SelectorFields&) const = default;
};

using BaseHeader = std::array<std::byte, BaseHeaderBytes>;
using TailHeader = std::array<std::byte, TailHeaderBytes>;
using Marker = std::array<std::byte, MarkerBytes>;
using Selector = std::array<std::byte, SelectorBytes>;
[[nodiscard]] BaseHeader encode_base_header(const BaseHeaderFields&);
[[nodiscard]] BaseHeaderFields decode_base_header(ByteView);
[[nodiscard]] TailHeader encode_tail_header(const TailHeaderFields&);
[[nodiscard]] TailHeaderFields decode_tail_header(ByteView);
// Without a supplied tail header, S and nonempty boundaries remain unverified.
[[nodiscard]] Marker encode_marker(const MarkerFields&);
[[nodiscard]] MarkerFields decode_marker(ByteView);
[[nodiscard]] Selector encode_selector(const SelectorFields&);
[[nodiscard]] SelectorFields decode_selector(ByteView);

// Re-encodes/validates fields; header CRC [0,88) differs from full-header CRC.
[[nodiscard]] TailHeaderIdentity tail_header_identity(const TailHeaderFields&);
[[nodiscard]] GenerationCheckpoint initial_checkpoint(const TailHeaderIdentity&);
// Checks immutable binding and the empty/nonempty marker rules achievable here.
// Does NOT find a record boundary or verify any tail bytes beyond its header.
void validate_marker_header(const MarkerFields&, const TailHeaderFields&);
// Only fixed-header consistency: no complete-base CRC, frames, tail scan, file
// identity, durability, selection, permission to serve, or writer activation.
void validate_header_links(const BaseHeaderFields&, const TailHeaderFields&, const MarkerFields&,
                           const SelectorFields&);

} // namespace redisdb::persistence::v2
