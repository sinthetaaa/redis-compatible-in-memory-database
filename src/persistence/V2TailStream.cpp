#include "redisdb/persistence/V2TailStream.hpp"

#include "redisdb/persistence/AofLog.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace redisdb::persistence::v2 {
namespace {
static_assert(std::numeric_limits<std::size_t>::max() <= UINT64_MAX);
static_assert(MaxTailPayload == MaxAofRecordBytes);
static_assert(std::is_nothrow_move_constructible_v<ValidatedTailScan>);
static_assert(std::is_nothrow_move_assignable_v<ValidatedTailScan>);
struct Rejection {
    TailStreamError error;
};
void require(bool ok, TailStreamError error) {
    if (!ok)
        throw Rejection{error}; // No allocation needed to report a framing failure.
}
std::uint64_t get(std::string_view bytes, std::size_t at, unsigned width) {
    require(at <= bytes.size() && width <= bytes.size() - at, TailStreamError::Frame);
    std::uint64_t n = 0;
    for (unsigned i = 0; i < width; ++i)
        n = (n << 8) | static_cast<unsigned char>(bytes[at + i]);
    return n;
}
GenerationCheckpoint checkpoint(const TailHeaderIdentity& identity, const SourceTuple& tuple) {
    return {identity, LogicalSequence{tuple.sequence}, TailOffset{tuple.offset},
            Crc32c{tuple.record_crc}, Crc32c{tuple.prefix_crc}};
}
} // namespace

ValidatedTailScan::ValidatedTailScan(TailHeaderFields header, GenerationCheckpoint observed,
                                     GenerationCheckpoint matched, std::uint64_t records) noexcept
    : header_(header), observed_(std::move(observed)), matched_(std::move(matched)),
      records_(records) {}

TailStreamValidator::TailStreamValidator(TailHeaderFields expected, MarkerFields marker,
                                         std::uint64_t extent, TailStreamLimits limits)
    : expected_(expected), marker_(marker), extent_(extent), limits_(limits) {
    if (extent_ < TailHeaderBytes || extent_ > MaxFileBytes) {
        (void)fail(TailStreamError::Length);
        return;
    }
    try {
        identity_ = tail_header_identity(expected_); // Revalidate untrusted expected fields.
    } catch (const FormatError&) {
        (void)fail(TailStreamError::Identity);
        return;
    } catch (const std::bad_alloc&) {
        (void)fail(TailStreamError::ResourceLimit);
        return;
    }
    try {
        validate_marker_header(marker_, expected_);
        require(marker_.durable.offset <= extent_, TailStreamError::Boundary);
    } catch (const FormatError&) {
        (void)fail(TailStreamError::Marker);
    } catch (const Rejection& e) {
        (void)fail(e.error);
    } catch (const std::bad_alloc&) {
        (void)fail(TailStreamError::ResourceLimit);
    }
}

StreamStatus TailStreamValidator::fail(TailStreamError error) noexcept {
    if (status_ == StreamStatus::Error)
        return status_;
    status_ = StreamStatus::Error;
    error_ = error;
    result_.reset();
    buffer_.reset();
    capacity_ = used_ = 0;
    identity_.reset();
    matched_ = false;
    return status_;
}
const ValidatedTailScan* TailStreamValidator::result() const noexcept {
    return status_ == StreamStatus::Complete ? &*result_ : nullptr;
}
void TailStreamValidator::grow(std::size_t size) {
    if (size <= capacity_)
        return;
    // Growth only occurs for the initial header or a checked 16-byte prelude.
    // Preserve that constant-sized prelude, release the old allocation first,
    // then allocate exactly the checked frame size. No geometric over-reserve
    // or simultaneous old/new record buffers when successive records grow.
    require(used_ <= 16 && size <= MaxTailRecordBytes, TailStreamError::ResourceLimit);
    std::array<char, 16> prelude{};
    if (used_)
        std::memcpy(prelude.data(), buffer_.get(), used_);
    buffer_.reset();
    capacity_ = 0;
    buffer_.reset(new char[size]);
    capacity_ = size;
    if (used_)
        std::memcpy(buffer_.get(), prelude.data(), used_);
}
void TailStreamValidator::match_boundary() {
    if (matched_)
        return;
    // Only invoked after accepting the actual header or a completely decoded
    // record. Crossing O, or reaching D with different coordinates, cannot be
    // repaired by any later record. A mere plausible byte offset never matches.
    if (observed_.sequence == marker_.durable.sequence ||
        observed_.offset >= marker_.durable.offset) {
        require(observed_ == marker_.durable, TailStreamError::Boundary);
        matched_ = true;
    }
}
void TailStreamValidator::accept_header() {
    const auto bytes = std::string_view{buffer_.get(), used_};
    const auto actual = decode_tail_header(std::as_bytes(std::span{bytes.data(), bytes.size()}));
    require(actual == expected_, TailStreamError::Identity);
    const auto actual_identity = tail_header_identity(actual);
    require(actual_identity == *identity_ &&
                get(bytes, 88, 4) == actual_identity.header_crc.value() &&
                crc32c(bytes) == actual_identity.full_header_crc.value(),
            TailStreamError::Identity);
    validate_marker_header(marker_, actual); // Binding to actual bytes, not claims alone.
    identity_ = actual_identity;
    observed_ = {actual.base.sequence, TailHeaderBytes, 0, actual_identity.full_header_crc.value()};
    match_boundary();
    header_accepted_ = true;
    used_ = 0;
    target_ = 16;
}
void TailStreamValidator::accept_record_header() {
    const auto bytes = std::string_view{buffer_.get(), used_};
    require(bytes.substr(0, 4) == "TXN1", TailStreamError::Frame);
    require(observed_.sequence != UINT64_MAX && get(bytes, 4, 8) == observed_.sequence + 1,
            TailStreamError::Sequence);
    const auto n = get(bytes, 12, 4);
    require(n <= MaxTailPayload, TailStreamError::Length);
    // received_ includes the prelude. Remaining extent must cover n + CRC/END!.
    // The checked extent <=INT64_MAX also proves record-end/received additions.
    require(received_ <= extent_ && extent_ - received_ >= 8 && n <= extent_ - received_ - 8,
            TailStreamError::Length);
    require(n <= limits_.record_payload && records_ < limits_.records && records_ != UINT64_MAX &&
                n <= std::numeric_limits<std::size_t>::max() - 24,
            TailStreamError::ResourceLimit);
    target_ = static_cast<std::size_t>(n) + 24;
    grow(target_);
    record_header_ = false;
}
void TailStreamValidator::accept_record() {
    const auto bytes = std::string_view{buffer_.get(), used_};
    const auto stored_crc = static_cast<std::uint32_t>(get(bytes, bytes.size() - 8, 4));
    require(stored_crc == crc32c(bytes.substr(0, bytes.size() - 8)), TailStreamError::Checksum);
    require(bytes.substr(bytes.size() - 4) == "END!", TailStreamError::Trailer);
    // Deliberately preserve journal LRU semantics: uniqueness/grammar only, no
    // base-snapshot permutation rule. Decode once and discard owned effects.
    (void)decode_effects(bytes.substr(16, bytes.size() - 24));
    observed_ = {get(bytes, 4, 8), received_, stored_crc,
                 extend_crc32c(observed_.prefix_crc, bytes)};
    ++records_; // Count/exhaustion/budget checked before buffering this record.
    match_boundary();
    used_ = 0;
    target_ = 16;
    record_header_ = true;
}

StreamStatus TailStreamValidator::feed(ByteView bytes) noexcept {
    if (status_ == StreamStatus::Error)
        return status_;
    if (status_ == StreamStatus::Complete)
        return fail(TailStreamError::Finalized);
    try {
        require(received_ <= extent_ && bytes.size() <= extent_ - received_,
                TailStreamError::Length);
        while (!bytes.empty()) {
            grow(target_);
            const auto n = std::min(bytes.size(), target_ - used_);
            std::memcpy(buffer_.get() + used_, bytes.data(), n);
            used_ += n;
            received_ += n;
            bytes = bytes.subspan(n);
            if (used_ != target_)
                continue;
            if (!header_accepted_)
                accept_header();
            else if (record_header_)
                accept_record_header();
            else
                accept_record();
        }
        return status_;
    } catch (const Rejection& e) {
        return fail(e.error);
    } catch (const FormatError&) {
        return fail(TailStreamError::Header);
    } catch (const PersistenceError&) {
        return fail(TailStreamError::Semantic);
    } catch (const std::bad_alloc&) {
        return fail(TailStreamError::ResourceLimit);
    } catch (const std::length_error&) {
        return fail(TailStreamError::ResourceLimit);
    }
}
StreamStatus TailStreamValidator::finish() noexcept {
    if (status_ != StreamStatus::NeedMore)
        return status_;
    if (!header_accepted_ || used_ != 0)
        return fail(TailStreamError::Truncated);
    if (received_ != extent_)
        return fail(TailStreamError::Length);
    if (!matched_)
        return fail(TailStreamError::Boundary);
    result_ = ValidatedTailScan{expected_, checkpoint(*identity_, observed_),
                                checkpoint(*identity_, marker_.durable), records_};
    status_ = StreamStatus::Complete;
    return status_;
}

} // namespace redisdb::persistence::v2
