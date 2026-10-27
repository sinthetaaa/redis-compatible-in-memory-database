#include "redisdb/persistence/V2Format.hpp"

#include "redisdb/persistence/AofLog.hpp"

#include <algorithm>
#include <bit>

namespace redisdb::persistence::v2 {
namespace {
void require(bool ok, Error error) {
    if (!ok)
        throw FormatError(error);
}
void room(ByteView bytes, std::size_t offset, std::size_t count) {
    require(offset <= bytes.size() && count <= bytes.size() - offset, Error::InvalidLength);
}
std::uint64_t get(ByteView bytes, std::size_t offset, unsigned width) {
    room(bytes, offset, width);
    std::uint64_t n = 0;
    for (unsigned i = 0; i < width; ++i)
        n = (n << 8) | std::to_integer<unsigned>(bytes[offset + i]);
    return n;
}
std::uint32_t get32(ByteView bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(get(bytes, offset, 4));
}
void put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t n, unsigned width) {
    room(bytes, offset, width);
    for (unsigned i = width; i != 0; --i) {
        bytes[offset + width - i] = static_cast<std::byte>((n >> ((i - 1) * 8)) & 255U);
    }
}
void text(std::span<std::byte> bytes, std::size_t offset, std::string_view value) {
    room(bytes, offset, value.size());
    for (std::size_t i = 0; i < value.size(); ++i)
        bytes[offset + i] = static_cast<std::byte>(value[i]);
}
bool matches(ByteView bytes, std::size_t offset, std::string_view value) {
    room(bytes, offset, value.size());
    for (std::size_t i = 0; i < value.size(); ++i)
        if (bytes[offset + i] != static_cast<std::byte>(value[i]))
            return false;
    return true;
}
void zero(ByteView bytes, std::size_t offset, std::size_t count) {
    room(bytes, offset, count);
    require(std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset + count),
                        [](auto b) { return b == std::byte{0}; }),
            Error::InvalidReserved);
}
StoreBytes store_at(ByteView bytes, std::size_t offset) {
    room(bytes, offset, 16);
    StoreBytes result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = bytes[offset + i];
    return result;
}
void store_at(std::span<std::byte> bytes, std::size_t offset, const StoreBytes& store) {
    room(bytes, offset, store.size());
    for (std::size_t i = 0; i < store.size(); ++i)
        bytes[offset + i] = store[i];
}
void identity(const StoreBytes& store, std::uint64_t generation) {
    (void)StoreId{store};
    (void)GenerationId{generation};
}
void accounting(std::uint32_t model) { require(model == 1, Error::InvalidAccounting); }
void base_reference(const BaseReferenceFields& b) {
    identity(b.store, b.generation);
    accounting(b.accounting_model);
    require(b.byte_length >= 204 && b.byte_length <= MaxFileBytes, Error::InvalidLength);
}
void tail_reference(const TailReferenceFields& t) {
    identity(t.store, t.generation);
    require(t.header_length == TailHeaderBytes, Error::InvalidLength);
}
void tuple_at(std::span<std::byte> bytes, std::size_t offset, const SourceTuple& t) {
    put(bytes, offset, t.sequence, 8);
    put(bytes, offset + 8, t.offset, 8);
    put(bytes, offset + 16, t.record_crc, 4);
    put(bytes, offset + 20, t.prefix_crc, 4);
}
SourceTuple tuple_at(ByteView bytes, std::size_t offset) {
    return {get(bytes, offset, 8), get(bytes, offset + 8, 8), get32(bytes, offset + 16),
            get32(bytes, offset + 20)};
}
// The V1 header is fixed; no source journal is opened to check an empty tuple.
std::uint32_t legacy_empty_prefix() {
    std::array<std::byte, 20> bytes{};
    text(bytes, 0, "RDBAOF01");
    put(bytes, 8, 1, 4);
    put(bytes, 16, checksum(ByteView{bytes}.first(16)).value(), 4);
    return checksum(bytes).value();
}
void legacy_tuple(const SourceTuple& t) {
    require(t.offset >= 20 && t.offset <= MaxFileBytes, Error::InvalidProvenance);
    if (t.sequence == 0)
        require(t.offset == 20 && t.record_crc == 0 && t.prefix_crc == legacy_empty_prefix(),
                Error::InvalidProvenance);
    else
        require(t.offset > 20, Error::InvalidProvenance);
}
void validate_base(const BaseHeaderFields& b) {
    identity(b.store, b.generation);
    accounting(b.accounting_model);
    require(b.flags <= 1, Error::UnsupportedFlags);
    require(b.lru_count == (b.flags ? b.key_count : 0), Error::InvalidCounts);
    // Necessary aggregate bounds only; actual frame/count/charge validation is deferred.
    constexpr auto max_body = MaxFileBytes - 204;
    require(b.key_count <= max_body / 35, Error::InvalidCounts);
    require(b.lru_count <= (max_body - 35 * b.key_count) / 16, Error::InvalidCounts);
    require(b.key_count <= UINT64_MAX / 64 && b.logical_charge >= 64 * b.key_count &&
                b.logical_charge <= std::numeric_limits<std::size_t>::max(),
            Error::InvalidCounts);
    const auto& p = b.source;
    require(p.kind <= 2, Error::InvalidProvenance);
    if (p.kind == 0) {
        require(p == ProvenanceFields{} && b.generation == 1 && b.sequence == 0 &&
                    b.key_count == 0 && b.logical_charge == 0 && b.lru_count == 0 && b.flags == 0,
                Error::InvalidProvenance);
    } else if (p.kind == 1) {
        require(b.generation == 1 && p.store == StoreBytes{} && p.generation == 0 &&
                    p.observed.sequence == b.sequence && p.durable.sequence <= b.sequence,
                Error::InvalidProvenance);
        legacy_tuple(p.observed);
        legacy_tuple(p.durable);
        require(p.durable.sequence == p.observed.sequence ? p.durable == p.observed
                                                          : p.durable.offset < p.observed.offset,
                Error::InvalidProvenance);
    } else {
        require(p.store == b.store && p.generation >= 1 && p.generation < b.generation &&
                    p.snapshot_generation == 0 && p.observed == p.durable &&
                    p.observed.sequence == b.sequence && p.observed.offset >= 96 &&
                    p.observed.offset <= MaxFileBytes,
                Error::InvalidProvenance);
        require(p.observed.offset != 96 || p.observed.record_crc == 0, Error::InvalidProvenance);
        require(b.sequence != 0 || (p.observed.offset == 96 && p.observed.record_crc == 0),
                Error::InvalidProvenance);
    }
}
void start(std::span<std::byte> bytes, std::string_view magic, std::uint32_t flags,
           std::uint32_t model) {
    text(bytes, 0, magic);
    put(bytes, 8, 2, 4);
    put(bytes, 12, flags, 4);
    put(bytes, 16, bytes.size(), 4);
    put(bytes, 20, model, 4);
}
void finish(std::span<std::byte> bytes) {
    put(bytes, bytes.size() - 8, checksum(ByteView{bytes}.first(bytes.size() - 8)).value(), 4);
    text(bytes, bytes.size() - 4, "END!");
}
void check_header(ByteView bytes, std::size_t size, std::string_view magic, bool lru) {
    require(bytes.size() == size, Error::InvalidLength);
    require(matches(bytes, 0, magic), Error::InvalidMagic);
    require(get32(bytes, 8) == 2, Error::UnsupportedVersion);
    require(get32(bytes, 12) <= (lru ? 1U : 0U), Error::UnsupportedFlags);
    require(get32(bytes, 16) == size, Error::InvalidLength);
    require(matches(bytes, size - 4, "END!"), Error::InvalidTrailer);
    require(get32(bytes, size - 8) == checksum(bytes.first(size - 8)).value(),
            Error::InvalidChecksum);
}
void base_at(std::span<std::byte> bytes, const BaseReferenceFields& b) {
    store_at(bytes, 24, b.store);
    put(bytes, 40, b.generation, 8);
    put(bytes, 48, b.sequence, 8);
    put(bytes, 56, b.byte_length, 8);
    put(bytes, 64, b.file_crc, 4);
    put(bytes, 68, b.header_crc, 4);
}
BaseReferenceFields base_at(ByteView bytes) {
    return {get32(bytes, 20),  store_at(bytes, 24), get(bytes, 40, 8), get(bytes, 48, 8),
            get(bytes, 56, 8), get32(bytes, 64),    get32(bytes, 68)};
}
void validate_tail_identity(const TailHeaderIdentity& t) {
    const auto& b = t.base;
    TailHeaderFields fields{{1, b.store.bytes(), b.generation.value(), b.sequence.value(),
                             b.byte_length, b.file_crc.value(), b.header_crc.value()}};
    const auto expected = tail_header_identity(fields);
    require(t == expected, Error::CrossHeaderMismatch);
}
} // namespace

FormatError::FormatError(Error code)
    : std::runtime_error("invalid V2 fixed metadata"), code_(code) {}
StoreId::StoreId(StoreBytes bytes) : bytes_(bytes) {
    require(bytes_ != StoreBytes{}, Error::InvalidIdentity);
}
GenerationId next_generation(GenerationId g) {
    require(g.value() != UINT64_MAX, Error::GenerationExhausted);
    return GenerationId{g.value() + 1};
}
LogicalSequence next_sequence(LogicalSequence s) {
    require(s.value() != UINT64_MAX, Error::SequenceExhausted);
    return LogicalSequence{s.value() + 1};
}
GenerationId parse_generation(std::string_view text) {
    require(!text.empty() && text.front() != '0', Error::InvalidIdentity);
    std::uint64_t n = 0;
    for (char c : text) {
        require(c >= '0' && c <= '9', Error::InvalidIdentity);
        const auto digit = static_cast<std::uint64_t>(c - '0');
        require(n <= (UINT64_MAX - digit) / 10, Error::InvalidIdentity);
        n = n * 10 + digit;
    }
    return GenerationId{n};
}
std::string format_generation(GenerationId g) { return std::to_string(g.value()); }
Crc32c checksum(ByteView bytes) noexcept {
    // Reuse accepted V1 CRC without reinterpreting storage or retaining a view.
    // Small fixed chunks keep arbitrary binary bytes exact and need no allocation.
    std::array<char, 184> buffer{};
    std::uint32_t crc = 0;
    while (!bytes.empty()) {
        const auto n = std::min(bytes.size(), buffer.size());
        for (std::size_t i = 0; i < n; ++i)
            buffer[i] = std::bit_cast<char>(std::to_integer<unsigned char>(bytes[i]));
        crc = extend_crc32c(crc, std::string_view{buffer.data(), n});
        bytes = bytes.subspan(n);
    }
    return Crc32c{crc};
}
BaseHeader encode_base_header(const BaseHeaderFields& b) {
    validate_base(b);
    BaseHeader bytes{};
    start(bytes, "RDBBAS02", b.flags, b.accounting_model);
    store_at(bytes, 24, b.store);
    put(bytes, 40, b.generation, 8);
    put(bytes, 48, b.sequence, 8);
    put(bytes, 56, b.key_count, 8);
    put(bytes, 64, b.logical_charge, 8);
    put(bytes, 72, b.lru_count, 8);
    put(bytes, 80, b.source.kind, 4);
    store_at(bytes, 88, b.source.store);
    put(bytes, 104, b.source.generation, 8);
    tuple_at(bytes, 112, b.source.observed);
    tuple_at(bytes, 136, b.source.durable);
    put(bytes, 160, b.source.snapshot_generation, 8);
    finish(bytes);
    return bytes;
}
BaseHeaderFields decode_base_header(ByteView bytes) {
    check_header(bytes, 184, "RDBBAS02", true);
    zero(bytes, 84, 4);
    zero(bytes, 168, 8);
    BaseHeaderFields b{get32(bytes, 12),
                       get32(bytes, 20),
                       store_at(bytes, 24),
                       get(bytes, 40, 8),
                       get(bytes, 48, 8),
                       get(bytes, 56, 8),
                       get(bytes, 64, 8),
                       get(bytes, 72, 8),
                       {get32(bytes, 80), store_at(bytes, 88), get(bytes, 104, 8),
                        tuple_at(bytes, 112), tuple_at(bytes, 136), get(bytes, 160, 8)}};
    validate_base(b);
    return b;
}
TailHeader encode_tail_header(const TailHeaderFields& t) {
    base_reference(t.base);
    TailHeader bytes{};
    start(bytes, "RDBTAL02", 0, t.base.accounting_model);
    base_at(bytes, t.base);
    finish(bytes);
    return bytes;
}
TailHeaderFields decode_tail_header(ByteView bytes) {
    check_header(bytes, 96, "RDBTAL02", false);
    zero(bytes, 72, 16);
    TailHeaderFields t{base_at(bytes)};
    base_reference(t.base);
    return t;
}
Marker encode_marker(const MarkerFields& m) {
    tail_reference(m.tail);
    (void)TailOffset{m.durable.offset};
    if (m.durable.offset == TailHeaderBytes)
        require(m.durable.record_crc == 0 && m.durable.prefix_crc == m.tail.full_header_crc,
                Error::InvalidBoundary);
    require(m.durable.sequence != 0 || m.durable.offset == TailHeaderBytes, Error::InvalidBoundary);
    Marker bytes{};
    start(bytes, "RDBCOM02", 0, 0);
    store_at(bytes, 24, m.tail.store);
    put(bytes, 40, m.tail.generation, 8);
    put(bytes, 48, m.tail.header_length, 4);
    put(bytes, 52, m.tail.header_crc, 4);
    put(bytes, 56, m.tail.full_header_crc, 4);
    tuple_at(bytes, 64, m.durable);
    finish(bytes);
    return bytes;
}
MarkerFields decode_marker(ByteView bytes) {
    check_header(bytes, 112, "RDBCOM02", false);
    zero(bytes, 20, 4);
    zero(bytes, 60, 4);
    zero(bytes, 88, 16);
    MarkerFields m{{store_at(bytes, 24), get(bytes, 40, 8), get32(bytes, 48), get32(bytes, 52),
                    get32(bytes, 56)},
                   tuple_at(bytes, 64)};
    (void)encode_marker(m);
    return m;
}
Selector encode_selector(const SelectorFields& s) {
    base_reference(s.base);
    require(s.tail_header_length == 96, Error::InvalidLength);
    Selector bytes{};
    start(bytes, "RDBCUR02", 0, s.base.accounting_model);
    base_at(bytes, s.base);
    put(bytes, 72, s.tail_header_length, 4);
    put(bytes, 76, s.tail_header_crc, 4);
    put(bytes, 80, s.tail_full_header_crc, 4);
    finish(bytes);
    return bytes;
}
SelectorFields decode_selector(ByteView bytes) {
    check_header(bytes, 112, "RDBCUR02", false);
    zero(bytes, 84, 20);
    SelectorFields s{base_at(bytes), get32(bytes, 72), get32(bytes, 76), get32(bytes, 80)};
    (void)encode_selector(s);
    return s;
}
TailHeaderIdentity tail_header_identity(const TailHeaderFields& fields) {
    const auto bytes = encode_tail_header(fields);
    const auto& b = fields.base;
    return {{StoreId{b.store}, GenerationId{b.generation}, LogicalSequence{b.sequence},
             b.byte_length, Crc32c{b.header_crc}, Crc32c{b.file_crc}},
            96,
            Crc32c{get32(bytes, 88)},
            checksum(bytes)};
}
GenerationCheckpoint initial_checkpoint(const TailHeaderIdentity& t) {
    validate_tail_identity(t);
    return {t, t.base.sequence, TailOffset{std::uint64_t{96}}, Crc32c{0U}, t.full_header_crc};
}
void validate_marker_header(const MarkerFields& m, const TailHeaderFields& fields) {
    (void)encode_marker(m);
    const auto t = tail_header_identity(fields);
    const TailReferenceFields expected{t.base.store.bytes(), t.base.generation.value(),
                                       t.header_length, t.header_crc.value(),
                                       t.full_header_crc.value()};
    require(m.tail == expected, Error::CrossHeaderMismatch);
    require(m.durable.sequence >= t.base.sequence.value(), Error::InvalidBoundary);
    if (m.durable.sequence == t.base.sequence.value())
        require(m.durable.offset == 96 && m.durable.record_crc == 0 &&
                    m.durable.prefix_crc == t.full_header_crc.value(),
                Error::InvalidBoundary);
    else
        require(m.durable.offset > 96, Error::InvalidBoundary);
}
void validate_header_links(const BaseHeaderFields& b, const TailHeaderFields& t,
                           const MarkerFields& m, const SelectorFields& s) {
    const auto base_bytes = encode_base_header(b);
    (void)encode_selector(s);
    require(t.base == s.base, Error::CrossHeaderMismatch);
    require(t.base.store == b.store && t.base.generation == b.generation &&
                t.base.sequence == b.sequence && t.base.accounting_model == b.accounting_model &&
                t.base.header_crc == get32(base_bytes, 176),
            Error::CrossHeaderMismatch);
    const auto tail = tail_header_identity(t);
    require(s.tail_header_length == tail.header_length &&
                s.tail_header_crc == tail.header_crc.value() &&
                s.tail_full_header_crc == tail.full_header_crc.value(),
            Error::CrossHeaderMismatch);
    // Necessary count bounds relative to the claimed base size, not its verified size.
    const auto body = t.base.byte_length - 204;
    require(b.key_count <= body / 35 && b.lru_count <= (body - 35 * b.key_count) / 16,
            Error::InvalidCounts);
    validate_marker_header(m, t);
}
} // namespace redisdb::persistence::v2
