#include "redisdb/persistence/V2BaseStream.hpp"

#include "redisdb/persistence/AofLog.hpp"
#include "redisdb/storage/MemoryAccounting.hpp"

#include <algorithm>
#include <new>
#include <type_traits>
#include <utility>

namespace redisdb::persistence::v2 {
namespace {
static_assert(std::numeric_limits<std::size_t>::max() <= UINT64_MAX);
static_assert(std::is_nothrow_move_constructible_v<ValidatedBase>);
static_assert(std::is_nothrow_move_assignable_v<ValidatedBase>);
void require(bool ok, BaseStreamError error) {
    if (!ok)
        throw BaseFrameError(error);
}
std::uint32_t get32(std::string_view bytes, std::size_t at) {
    require(at <= bytes.size() && 4 <= bytes.size() - at, BaseStreamError::Length);
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i)
        n = (n << 8) | static_cast<unsigned char>(bytes[at + i]);
    return n;
}
void put32(std::string& bytes, std::uint32_t n) {
    for (unsigned i = 4; i != 0; --i)
        bytes.push_back(static_cast<char>((n >> ((i - 1) * 8)) & 255U));
}
std::string frame(std::string_view tag, std::string_view payload, std::size_t cap) {
    require(payload.size() <= cap, BaseStreamError::Length);
    std::string bytes;
    require(bytes.max_size() >= 16 && payload.size() <= bytes.max_size() - 16,
            BaseStreamError::ResourceLimit);
    bytes.reserve(payload.size() + 16);
    bytes.append(tag);
    put32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.append(payload);
    put32(bytes, crc32c(bytes));
    bytes.append("END!");
    return bytes;
}
} // namespace

ValidatedBase::ValidatedBase(BaseIdentity identity, BaseHeaderFields header,
                             std::vector<storage::EntryEffect> entries,
                             std::vector<std::size_t> lru, std::uint32_t prefix, std::uint32_t done)
    : identity_(std::move(identity)), header_(header), entries_(std::move(entries)),
      lru_(std::move(lru)), prefix_crc_(prefix), done_crc_(done) {}

bool BaseStreamValidator::UnsignedLess::operator()(const std::string& a,
                                                   const std::string& b) const noexcept {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](unsigned char x, unsigned char y) { return x < y; });
}

BaseStreamValidator::BaseStreamValidator(BaseReferenceFields expected, BaseStreamLimits limits)
    : expected_(expected), limits_(limits) {
    try {
        // Existing pure fixed codec validates all untrusted reference fields.
        (void)encode_tail_header(TailHeaderFields{expected_});
    } catch (const FormatError&) {
        (void)fail(BaseStreamError::Identity);
    } catch (const std::bad_alloc&) {
        (void)fail(BaseStreamError::ResourceLimit);
    }
}
BaseStreamValidator::BaseStreamValidator(const BaseIdentity& expected, BaseStreamLimits limits)
    : BaseStreamValidator(
          BaseReferenceFields{1, expected.store.bytes(), expected.generation.value(),
                              expected.sequence.value(), expected.byte_length,
                              expected.file_crc.value(), expected.header_crc.value()},
          limits) {}

StreamStatus BaseStreamValidator::fail(BaseStreamError error) noexcept {
    if (status_ == StreamStatus::Error)
        return status_;
    status_ = StreamStatus::Error;
    error_ = error;
    result_.reset();
    std::string{}.swap(buffer_);
    std::vector<storage::EntryEffect>{}.swap(entries_);
    std::vector<std::size_t>{}.swap(lru_);
    remaining_.clear();
    header_.reset();
    return status_;
}
const ValidatedBase* BaseStreamValidator::result() const noexcept {
    return status_ == StreamStatus::Complete ? &*result_ : nullptr;
}

void BaseStreamValidator::accept_header() {
    header_ = decode_base_header(std::as_bytes(std::span{buffer_.data(), buffer_.size()}));
    const auto& h = *header_;
    require(h.store == expected_.store && h.generation == expected_.generation &&
                h.sequence == expected_.sequence &&
                h.accounting_model == expected_.accounting_model &&
                get32(buffer_, 176) == expected_.header_crc,
            BaseStreamError::Identity);
    const auto body = expected_.byte_length - 204; // reference already checked
    require(h.key_count <= body / 35 && h.lru_count <= (body - 35 * h.key_count) / 16,
            BaseStreamError::Length);
    require(h.key_count <= entries_.max_size() && h.lru_count <= lru_.max_size() &&
                h.key_count <= limits_.entries && h.logical_charge <= limits_.logical_charge,
            BaseStreamError::ResourceLimit);
    buffer_.clear();
    target_ = 8;
}
void BaseStreamValidator::accept_frame_header() {
    const auto tag = std::string_view{buffer_}.substr(0, 4);
    const auto expected_tag = keys_ < header_->key_count   ? "KEY1"
                              : lrus_ < header_->lru_count ? "LRU1"
                                                           : "DONE";
    require(tag == expected_tag, BaseStreamError::Frame);
    const auto n = get32(buffer_, 4);
    const auto cap = tag == "KEY1" ? MaxKeyPayload : tag == "LRU1" ? MaxLruPayload : 4;
    require(n <= cap && (tag != "DONE" || n == 4), BaseStreamError::Length);
    // received_ already includes the eight-byte frame header. Prove room for
    // payload + checksum + trailer using subtraction before endpoint arithmetic.
    require(received_ <= expected_.byte_length && expected_.byte_length - received_ >= 8 &&
                n <= expected_.byte_length - received_ - 8,
            BaseStreamError::Length);
    require(n <= limits_.frame_payload && buffer_.max_size() >= 16 && n <= buffer_.max_size() - 16,
            BaseStreamError::ResourceLimit);
    target_ = static_cast<std::size_t>(n) + 16;
    buffer_.reserve(target_);
    frame_header_ = false;
}
void BaseStreamValidator::accept_frame() {
    const auto bytes = std::string_view{buffer_};
    const auto payload = bytes.substr(8, bytes.size() - 16);
    const auto stored_crc = get32(bytes, bytes.size() - 8);
    require(stored_crc == crc32c(bytes.substr(0, bytes.size() - 8)), BaseStreamError::Checksum);
    require(bytes.substr(bytes.size() - 4) == "END!", BaseStreamError::Trailer);
    if (keys_ < header_->key_count) {
        auto effects = decode_effects(payload);
        require(effects.entries.size() == 1 && effects.entries.front().replacement &&
                    !effects.has_lru && effects.lru_order.empty(),
                BaseStreamError::Semantic);
        auto& effect = effects.entries.front();
        require(entries_.empty() || UnsignedLess{}(entries_.back().key, effect.key),
                BaseStreamError::Order);
        auto& entry = *effect.replacement;
        entry.accounted_charge = storage::memory_accounting::entry_charge(effect.key, entry);
        charge_ = storage::memory_accounting::checked_add(charge_, entry.accounted_charge);
        require(charge_ <= limits_.logical_charge, BaseStreamError::ResourceLimit);
        if (header_->flags)
            remaining_.emplace(effect.key, entries_.size());
        entries_.push_back(std::move(effect));
        ++keys_; // bounded by the checked header count
    } else if (lrus_ < header_->lru_count) {
        const auto found = remaining_.find(std::string{payload});
        require(found != remaining_.end(), BaseStreamError::Permutation);
        lru_.push_back(found->second);
        remaining_.erase(found);
        ++lrus_;
    } else {
        require(get32(payload, 0) == prefix_crc_, BaseStreamError::Checksum);
        require(remaining_.empty(), BaseStreamError::Permutation);
        require(charge_ == header_->logical_charge, BaseStreamError::Charge);
        done_crc_ = stored_crc;
        done_ = true;
    }
    buffer_.clear();
    target_ = 8;
    frame_header_ = true;
}

StreamStatus BaseStreamValidator::feed(ByteView bytes) noexcept {
    if (status_ == StreamStatus::Error)
        return status_;
    if (status_ == StreamStatus::Complete)
        return fail(BaseStreamError::Finalized);
    try {
        while (!bytes.empty()) {
            require(!done_, BaseStreamError::TrailingBytes);
            require(received_ <= expected_.byte_length &&
                        bytes.size() <= expected_.byte_length - received_,
                    BaseStreamError::Length);
            if (header_ && frame_header_ && buffer_.empty())
                prefix_crc_ = file_crc_;
            const auto n = std::min(bytes.size(), target_ - buffer_.size());
            const std::string_view piece{reinterpret_cast<const char*>(bytes.data()), n};
            buffer_.append(piece);
            file_crc_ = extend_crc32c(file_crc_, piece);
            received_ += n;
            bytes = bytes.subspan(n);
            if (buffer_.size() != target_)
                continue;
            if (!header_)
                accept_header();
            else if (frame_header_)
                accept_frame_header();
            else
                accept_frame();
        }
        return status_;
    } catch (const BaseFrameError& error) {
        return fail(error.code());
    } catch (const FormatError&) {
        return fail(BaseStreamError::Header);
    } catch (const PersistenceError&) {
        return fail(BaseStreamError::Semantic);
    } catch (const std::overflow_error&) {
        return fail(BaseStreamError::Charge);
    } catch (const std::bad_alloc&) {
        return fail(BaseStreamError::ResourceLimit);
    } catch (const std::length_error&) {
        return fail(BaseStreamError::ResourceLimit);
    }
}
StreamStatus BaseStreamValidator::finish() noexcept {
    if (status_ != StreamStatus::NeedMore)
        return status_;
    if (!done_)
        return fail(BaseStreamError::Truncated);
    if (received_ != expected_.byte_length)
        return fail(BaseStreamError::Length);
    if (file_crc_ != expected_.file_crc)
        return fail(BaseStreamError::Identity);
    result_ = ValidatedBase{BaseIdentity{StoreId{header_->store}, GenerationId{header_->generation},
                                         LogicalSequence{header_->sequence}, received_,
                                         Crc32c{expected_.header_crc}, Crc32c{file_crc_}},
                            *header_,
                            std::move(entries_),
                            std::move(lru_),
                            prefix_crc_,
                            done_crc_};
    status_ = StreamStatus::Complete;
    return status_;
}

std::string encode_key_frame(const storage::EntryEffect& replacement) {
    require(replacement.replacement.has_value(), BaseStreamError::Semantic);
    storage::CommittedEffects effects;
    effects.entries.push_back(replacement);
    const auto payload = encode_effects(effects);
    // V1 writer can encode empty collections from internal nonpersistent state;
    // the matching accepted decoder rejects these. Never emit an invalid KEY1.
    (void)decode_effects(payload);
    return frame("KEY1", payload, MaxKeyPayload);
}
std::string encode_lru_frame(std::string_view key) { return frame("LRU1", key, MaxLruPayload); }
std::string encode_done_frame(Crc32c prefix) {
    std::string payload;
    put32(payload, prefix.value());
    return frame("DONE", payload, 4);
}
} // namespace redisdb::persistence::v2
