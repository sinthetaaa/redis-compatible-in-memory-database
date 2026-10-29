#include "redisdb/persistence/V2BaseEncoding.hpp"

#include "redisdb/persistence/AofLog.hpp"
#include "redisdb/storage/MemoryAccounting.hpp"

#include <algorithm>
#include <chrono>
#include <new>
#include <type_traits>
#include <utility>

namespace redisdb::persistence::v2 {
namespace {
static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
constexpr std::size_t MaxItems = 1'048'576;
bool check(bool ok, BaseEncodeError code, BaseEncodeError& first) noexcept {
    if (!ok && first == BaseEncodeError::None)
        first = code;
    return ok;
}
constexpr bool room(std::uint64_t a, std::uint64_t b, std::uint64_t ceiling) noexcept {
    return a <= ceiling && b <= ceiling - a;
}
// Exercise the actual subtraction guard without fabricating invalid giant spans.
static_assert(room(0, UINT64_MAX, UINT64_MAX));
static_assert(room(UINT64_MAX, 0, UINT64_MAX));
static_assert(!room(UINT64_MAX, 1, UINT64_MAX));
static_assert(!room(MaxFileBytes, 1, MaxFileBytes));
static_assert(!room(UINT64_MAX, 0, MaxFileBytes));
bool less(std::string_view a, std::string_view b) noexcept {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](unsigned char x, unsigned char y) { return x < y; });
}
bool payload_add(std::uint64_t& total, std::uint64_t n, BaseEncodeError& error) noexcept {
    if (!check(room(total, n, MaxKeyPayload), BaseEncodeError::EntryEncoding, error))
        return false;
    total += n;
    return true;
}
bool blob_size(std::uint64_t& total, std::string_view bytes, BaseEncodeError& error) noexcept {
    return check(bytes.size() <= MaxLruPayload, BaseEncodeError::EntryEncoding, error) &&
           payload_add(total, 4, error) && payload_add(total, bytes.size(), error);
}
bool collection_size(std::uint64_t& total, std::size_t count, BaseEncodeError& error) noexcept {
    return check(count != 0 && count <= MaxItems, BaseEncodeError::EntryEncoding, error) &&
           payload_add(total, 4, error);
}
bool payload_size(const BaseEntryView& view, std::uint64_t& total,
                  BaseEncodeError& error) noexcept {
    if (!check(view.entry != nullptr && !view.entry->value.valueless_by_exception(),
               BaseEncodeError::InvalidInput, error))
        return false;
    const auto& entry = *view.entry;
    total = 4; // Exactly one replacement.
    if (!blob_size(total, view.key, error) ||
        !payload_add(total, 7, error)) // tag, timed, has_lru=0 and LRU count=0.
        return false;
    if (entry.expires_at) {
        using Duration = storage::WallTimePoint::duration;
        static_assert(Duration::period::num == 1 && Duration::period::den % 1'000'000 == 0);
        constexpr std::int64_t scale = Duration::period::den / 1'000'000;
        const auto us =
            std::chrono::floor<std::chrono::microseconds>(entry.expires_at->time_since_epoch())
                .count();
        if (!check(us >= INT64_MIN / scale && us <= INT64_MAX / scale,
                   BaseEncodeError::EntryEncoding, error) ||
            !payload_add(total, 8, error))
            return false;
    }
    // Pointer queries do not throw for a different alternative. No visitor
    // exception is needed to reject an invalid/valueless physical entry.
    static_assert(std::variant_size_v<storage::Value> == 4);
    if (const auto* string = std::get_if<storage::StringValue>(&entry.value)) {
        return blob_size(total, string->bytes, error);
    }
    if (const auto* list = std::get_if<storage::ListValue>(&entry.value)) {
        if (!collection_size(total, list->elements.size(), error))
            return false;
        for (const auto& item : list->elements)
            if (!blob_size(total, item, error))
                return false;
        return true;
    }
    if (const auto* hash = std::get_if<storage::HashValue>(&entry.value)) {
        if (!collection_size(total, hash->fields.size(), error))
            return false;
        for (const auto& [key, bytes] : hash->fields)
            if (!blob_size(total, key, error) || !blob_size(total, bytes, error))
                return false;
        return true;
    }
    const auto* set = std::get_if<storage::SetValue>(&entry.value);
    if (!check(set != nullptr, BaseEncodeError::InvalidInput, error) ||
        !collection_size(total, set->members.size(), error))
        return false;
    for (const auto& item : set->members)
        if (!blob_size(total, item, error))
            return false;
    return true;
}
bool extent_add(std::uint64_t& total, std::uint64_t n, std::uint64_t budget,
                BaseEncodeError& error) noexcept {
    if (!check(room(total, n, MaxFileBytes), BaseEncodeError::ExtentOverflow, error) ||
        !check(room(total, n, budget), BaseEncodeError::ResourceLimit, error))
        return false;
    total += n;
    return true;
}
std::uint32_t get32(std::string_view bytes, std::size_t offset) noexcept {
    std::uint32_t n = 0;
    for (std::size_t i = 0; i < 4; ++i)
        n = (n << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return n;
}
} // namespace

EncodedBaseSummary::EncodedBaseSummary(BaseIdentity identity, BaseHeaderFields header,
                                       Crc32c prefix, Crc32c done) noexcept
    : identity_(std::move(identity)), header_(header), prefix_(prefix), done_(done) {}
const BaseIdentity& EncodedBaseSummary::identity() const noexcept { return identity_; }
const BaseHeaderFields& EncodedBaseSummary::header() const noexcept { return header_; }
Crc32c EncodedBaseSummary::prefix_crc() const noexcept { return prefix_; }
Crc32c EncodedBaseSummary::done_crc() const noexcept { return done_; }

BaseEncodingCursor::BaseEncodingCursor() noexcept = default;
BaseEncodingCursor::~BaseEncodingCursor() noexcept = default;
BaseEncodingCursor::BaseEncodingCursor(BaseEncodingCursor&& other) noexcept {
    *this = std::move(other);
}
BaseEncodingCursor& BaseEncodingCursor::operator=(BaseEncodingCursor&& other) noexcept {
    if (this != &other) {
        phase_ = std::exchange(other.phase_, Phase::MovedFrom);
        context_ = std::exchange(other.context_, std::nullopt);
        input_ = std::exchange(other.input_, BaseInputView{});
        limits_ = other.limits_;
        header_ = other.header_;
        header_bytes_ = other.header_bytes_;
        seen_ = std::move(other.seen_);
        frame_ = std::move(other.frame_);
        key_index_ = std::exchange(other.key_index_, 0);
        lru_index_ = std::exchange(other.lru_index_, 0);
        expected_extent_ = std::exchange(other.expected_extent_, 0);
        emitted_ = std::exchange(other.emitted_, 0);
        prefix_ = other.prefix_;
        done_ = other.done_;
        file_crc_ = other.file_crc_;
        chunk_live_ = std::exchange(other.chunk_live_, false);
        header_chunk_ = std::exchange(other.header_chunk_, false);
        failure_ = std::exchange(other.failure_, BaseEncodeFailure{});
        result_ = std::exchange(other.result_, std::nullopt);
        other.frame_.clear();
        other.seen_.clear();
    }
    return *this;
}
BaseEncodingCursor BaseEncodingCursor::begin(BaseEncodingContext context, BaseInputView input,
                                             BaseEncodeLimits limits) noexcept {
    BaseEncodingCursor cursor;
    const auto fail = [&](BaseEncodeFailure failure) noexcept {
        cursor.failure_ = failure;
        cursor.phase_ = Phase::Failed;
    };
    BaseEncodeError rejection = BaseEncodeError::None;
    // This helper may invoke unchanged throwing codecs/containers. Local checks
    // return false with a scalar error; only dependency exceptions reach catches.
    const auto initialize = [&]() -> bool {
        if (!check(limits.max_bytes >= 204 && limits.max_bytes <= MaxFileBytes &&
                       limits.max_frame_payload >= 4 && limits.max_frame_payload <= MaxKeyPayload,
                   BaseEncodeError::ResourceLimit, rejection))
            return false;
        if (!check(context.candidate_generation.value() > context.selected_generation.value() &&
                       context.captured_sequence.value() != UINT64_MAX &&
                       context.synchronized_source.sequence == context.captured_sequence.value() &&
                       context.synchronized_source.offset >= TailHeaderBytes &&
                       context.synchronized_source.offset <= MaxFileBytes &&
                       (context.synchronized_source.offset != TailHeaderBytes ||
                        context.synchronized_source.record_crc == 0) &&
                       (context.captured_sequence.value() != 0 ||
                        (context.synchronized_source.offset == TailHeaderBytes &&
                         context.synchronized_source.record_crc == 0)),
                   BaseEncodeError::InvalidContext, rejection))
            return false;
        if (!check(input.entries.size() <= limits.max_entries && cursor.frame_.max_size() >= 20,
                   BaseEncodeError::ResourceLimit, rejection))
            return false;
        std::size_t charge = 0;
        std::uint64_t extent = 204; // Header + final DONE reserved before every frame.
        for (std::size_t i = 0; i < input.entries.size(); ++i) {
            const auto& entry = input.entries[i];
            if (!check(entry.entry != nullptr, BaseEncodeError::InvalidInput, rejection) ||
                !check(i == 0 || less(input.entries[i - 1].key, entry.key),
                       BaseEncodeError::KeyOrder, rejection))
                return false;
            std::uint64_t payload = 0;
            if (!payload_size(entry, payload, rejection) ||
                !check(payload <= limits.max_frame_payload &&
                           payload <= cursor.frame_.max_size() - 16,
                       BaseEncodeError::ResourceLimit, rejection) ||
                !extent_add(extent, payload + 16, limits.max_bytes, rejection))
                return false;
            const auto entry_charge =
                storage::memory_accounting::entry_charge(entry.key, *entry.entry);
            if (!check(room(charge, entry_charge, SIZE_MAX), BaseEncodeError::ChargeOverflow,
                       rejection))
                return false;
            charge = storage::memory_accounting::checked_add(charge, entry_charge);
            if (!check(charge <= limits.max_logical_charge, BaseEncodeError::ResourceLimit,
                       rejection))
                return false;
        }
        if (!check(input.include_lru || input.lru_indices.empty(), BaseEncodeError::Permutation,
                   rejection))
            return false;
        if (input.include_lru) {
            const auto k = input.entries.size();
            if (!check(input.lru_indices.size() == k, BaseEncodeError::Permutation, rejection))
                return false;
            const auto bitmap = k / 8 + static_cast<std::size_t>(k % 8 != 0);
            if (!check(bitmap <= limits.max_permutation_bytes && bitmap <= cursor.seen_.max_size(),
                       BaseEncodeError::ResourceLimit, rejection))
                return false;
            cursor.seen_.resize(bitmap);
            for (const auto index : input.lru_indices) {
                if (!check(index < k, BaseEncodeError::Permutation, rejection))
                    return false;
                auto& cell = cursor.seen_[index / 8];
                const auto mask = static_cast<std::byte>(1U << (index % 8));
                if (!check((cell & mask) == std::byte{0}, BaseEncodeError::Permutation, rejection))
                    return false;
                cell |= mask;
                const auto size = input.entries[index].key.size();
                if (!check(size <= MaxLruPayload, BaseEncodeError::EntryEncoding, rejection) ||
                    !check(size <= limits.max_frame_payload &&
                               size <= cursor.frame_.max_size() - 16,
                           BaseEncodeError::ResourceLimit, rejection) ||
                    !extent_add(extent, size + 16, limits.max_bytes, rejection))
                    return false;
            }
        }
        cursor.header_ = {input.include_lru ? 1U : 0U,
                          1,
                          context.store.bytes(),
                          context.candidate_generation.value(),
                          context.captured_sequence.value(),
                          input.entries.size(),
                          charge,
                          input.include_lru ? input.entries.size() : 0,
                          {2, context.store.bytes(), context.selected_generation.value(),
                           context.synchronized_source, context.synchronized_source, 0}};
        cursor.header_bytes_ = encode_base_header(cursor.header_);
        cursor.context_ = context;
        cursor.input_ = input;
        cursor.limits_ = limits;
        cursor.expected_extent_ = extent;
        cursor.phase_ = Phase::Header;
        return true;
    };
    try {
        if (!initialize())
            fail({rejection, {}, {}});
    } catch (const FormatError& e) {
        fail({BaseEncodeError::InvalidContext, e.code(), {}});
    } catch (const std::overflow_error&) {
        fail({BaseEncodeError::ChargeOverflow, {}, {}});
    } catch (const std::bad_alloc&) {
        fail({BaseEncodeError::ResourceLimit, {}, {}});
    } catch (const std::length_error&) {
        fail({BaseEncodeError::ResourceLimit, {}, {}});
    }
    return cursor;
}
BaseEncodeStep BaseEncodingCursor::next() noexcept {
    chunk_live_ = false;
    header_chunk_ = false;
    if (phase_ == Phase::Complete)
        return BaseEncodeStep::End;
    if (phase_ == Phase::Empty || phase_ == Phase::MovedFrom || phase_ == Phase::Failed)
        return BaseEncodeStep::Error;
    const auto fail = [&](BaseEncodeFailure failure) noexcept {
        if (phase_ != Phase::Failed) {
            failure_ = failure;
            phase_ = Phase::Failed;
            result_.reset();
            frame_.clear();
        }
        return BaseEncodeStep::Error;
    };
    try {
        frame_.clear();
        if (phase_ == Phase::Header) {
            header_chunk_ = true;
            phase_ = Phase::Keys;
        } else {
            if (phase_ == Phase::Keys && key_index_ == input_.entries.size())
                phase_ = Phase::Lru;
            if (phase_ == Phase::Lru && lru_index_ == input_.lru_indices.size())
                phase_ = Phase::Done;
            if (phase_ == Phase::Keys) {
                const auto& view = input_.entries[key_index_];
                std::uint64_t payload = 0;
                BaseEncodeError rejection = BaseEncodeError::None;
                if (!payload_size(view, payload, rejection))
                    return fail({rejection, {}, {}});
                if (payload > limits_.max_frame_payload)
                    return fail({BaseEncodeError::ResourceLimit, {}, {}});
                storage::Entry entry{view.entry->value, view.entry->expires_at};
                frame_ =
                    encode_key_frame(storage::EntryEffect{std::string{view.key}, std::move(entry)});
                if (frame_.size() != payload + 16)
                    return fail({BaseEncodeError::EntryEncoding, {}, {}});
                ++key_index_;
            } else if (phase_ == Phase::Lru) {
                frame_ = encode_lru_frame(input_.entries[input_.lru_indices[lru_index_]].key);
                ++lru_index_;
            } else {
                // Done phase is visited once for its chunk, then once for End.
                if (emitted_ == expected_extent_) {
                    const auto& c = *context_;
                    result_ = EncodedBaseSummary{
                        BaseIdentity{c.store, c.candidate_generation, c.captured_sequence, emitted_,
                                     checksum(ByteView{header_bytes_}.first(176)),
                                     Crc32c{file_crc_}},
                        header_, Crc32c{prefix_}, Crc32c{done_}};
                    phase_ = Phase::Complete;
                    return BaseEncodeStep::End;
                }
                prefix_ = file_crc_;
                frame_ = encode_done_frame(Crc32c{prefix_});
                if (!room(emitted_, frame_.size(), expected_extent_) ||
                    emitted_ + frame_.size() != expected_extent_)
                    return fail({BaseEncodeError::ExtentOverflow, {}, {}});
                done_ = get32(frame_, frame_.size() - 8);
            }
        }
        const auto chunk = header_chunk_ ? ByteView{header_bytes_}
                                         : std::as_bytes(std::span{frame_.data(), frame_.size()});
        if (!room(emitted_, chunk.size(), expected_extent_))
            return fail({BaseEncodeError::ExtentOverflow, {}, {}});
        file_crc_ = extend_crc32c(
            file_crc_, std::string_view{reinterpret_cast<const char*>(chunk.data()), chunk.size()});
        emitted_ += chunk.size();
        chunk_live_ = true;
        return BaseEncodeStep::Chunk;

    } catch (const FormatError& e) {
        return fail({BaseEncodeError::EntryEncoding, e.code(), {}});
    } catch (const BaseFrameError& e) {
        return fail({BaseEncodeError::EntryEncoding, {}, e.code()});
    } catch (const PersistenceError&) {
        return fail({BaseEncodeError::EntryEncoding, {}, {}});
    } catch (const std::overflow_error&) {
        return fail({BaseEncodeError::ChargeOverflow, {}, {}});
    } catch (const std::bad_alloc&) {
        return fail({BaseEncodeError::ResourceLimit, {}, {}});
    } catch (const std::length_error&) {
        return fail({BaseEncodeError::ResourceLimit, {}, {}});
    }
}
ByteView BaseEncodingCursor::bytes() const noexcept {
    if (!chunk_live_)
        return {};
    return header_chunk_ ? ByteView{header_bytes_}
                         : std::as_bytes(std::span{frame_.data(), frame_.size()});
}
bool BaseEncodingCursor::complete() const noexcept { return phase_ == Phase::Complete; }
BaseEncodeFailure BaseEncodingCursor::error() const noexcept {
    return phase_ == Phase::Empty || phase_ == Phase::MovedFrom
               ? BaseEncodeFailure{BaseEncodeError::Closed, {}, {}}
               : failure_;
}
std::uint64_t BaseEncodingCursor::bytes_emitted() const noexcept { return emitted_; }
const EncodedBaseSummary* BaseEncodingCursor::result() const noexcept {
    return complete() ? &*result_ : nullptr;
}
} // namespace redisdb::persistence::v2
