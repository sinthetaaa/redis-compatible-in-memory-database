#include "redisdb/persistence/V2Format.hpp"

#include "redisdb/persistence/AofLog.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <type_traits>
#include <vector>

namespace {
using namespace redisdb::persistence::v2;
static_assert(!std::is_default_constructible_v<StoreId>);
static_assert(!std::is_default_constructible_v<GenerationId>);
static_assert(!std::is_default_constructible_v<GenerationCheckpoint>);
static_assert(!std::is_convertible_v<std::uint64_t, GenerationId>);
static_assert(!std::is_convertible_v<int, GenerationId>);
static_assert(!std::is_constructible_v<GenerationId, int>);
static_assert(!std::is_constructible_v<Crc32c, std::uint64_t>);
static_assert(!std::is_constructible_v<GenerationId, double>);
static_assert(!std::is_default_constructible_v<TailOffset>);
static_assert(!std::is_constructible_v<GenerationCheckpoint, redisdb::persistence::AofCheckpoint>);
static_assert(!std::is_constructible_v<redisdb::persistence::AofCheckpoint, GenerationCheckpoint>);
static_assert(!std::is_convertible_v<LogicalSequence, TailOffset>);

void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
template <class Fn> void fails(Error code, Fn fn) {
    try {
        fn();
    } catch (const FormatError& e) {
        check(e.code() == code, "wrong V2 error category");
        return;
    }
    throw std::runtime_error("expected V2 format rejection");
}
std::vector<std::byte> hex(std::string_view value) {
    std::vector<std::byte> result;
    check(value.size() % 2 == 0, "odd hex");
    auto digit = [](char c) -> unsigned {
        return static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    for (std::size_t i = 0; i < value.size(); i += 2)
        result.push_back(static_cast<std::byte>(digit(value[i]) * 16 + digit(value[i + 1])));
    return result;
}
void number(std::span<std::byte> bytes, std::size_t offset, std::uint64_t n, unsigned width) {
    check(offset <= bytes.size() && width <= bytes.size() - offset, "test offset");
    for (unsigned i = width; i != 0; --i)
        bytes[offset + width - i] = static_cast<std::byte>((n >> ((i - 1) * 8)) & 255U);
}
void reseal(std::span<std::byte> bytes) {
    number(bytes, bytes.size() - 8, checksum(ByteView{bytes}.first(bytes.size() - 8)).value(), 4);
}
template <class Bytes> void equal(const Bytes& actual, ByteView expected) {
    check(actual.size() == expected.size() &&
              std::equal(actual.begin(), actual.end(), expected.begin()),
          "golden bytes differ");
}

// Fixed portions of the accepted section-15 Python recipe, reproduced in memory.
// No base frame or transaction codec is exercised or implemented by this test.
struct Golden {
    std::string_view base, tail, current, marker;
};
const Golden golden[] = {
    {
        // E
        "52444242415330320000000200000000000000b800000001000102030405060708090a0b0c0d0e0f"
        "00000000000000010000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000bc252022454e4421",
        "52444254414c303200000002000000000000006000000001000102030405060708090a0b0c0d0e0f"
        "0000000000000001000000000000000000000000000000cc4ebad660bc2520220000000000000000"
        "0000000000000000cc38e707454e4421",
        "524442435552303200000002000000000000007000000001000102030405060708090a0b0c0d0e0f"
        "0000000000000001000000000000000000000000000000cc4ebad660bc25202200000060cc38e707"
        "dbfbc819000000000000000000000000000000000000000015208444454e4421",
        "524442434f4d303200000002000000000000007000000000000102030405060708090a0b0c0d0e0f"
        "000000000000000100000060cc38e707dbfbc8190000000000000000000000000000000000000060"
        "00000000dbfbc81900000000000000000000000000000000bba4806d454e4421",
    },
    {
        // C
        "52444242415330320000000200000000000000b800000001000102030405060708090a0b0c0d0e0f"
        "00000000000000030000000000000001000000000000000100000000000000450000000000000000"
        "0000000200000000000102030405060708090a0b0c0d0e0f00000000000000010000000000000001"
        "0000000000000090ad924c7404430f7400000000000000010000000000000090ad924c7404430f74"
        "00000000000000000000000000000000213707da454e4421",
        "52444254414c303200000002000000000000006000000001000102030405060708090a0b0c0d0e0f"
        "0000000000000003000000000000000100000000000000f4155d9af1213707da0000000000000000"
        "0000000000000000ad63e9bb454e4421",
        "524442435552303200000002000000000000007000000001000102030405060708090a0b0c0d0e0f"
        "0000000000000003000000000000000100000000000000f4155d9af1213707da00000060ad63e9bb"
        "28b640b9000000000000000000000000000000000000000057507825454e4421",
        "524442434f4d303200000002000000000000007000000000000102030405060708090a0b0c0d0e0f"
        "000000000000000300000060ad63e9bb28b640b90000000000000000000000010000000000000060"
        "0000000028b640b900000000000000000000000000000000c81f9e4b454e4421",
    },
    {
        // L
        "52444242415330320000000200000001000000b800000001000102030405060708090a0b0c0d0e0f"
        "00000000000000040000000000000002000000000000000200000000000000870000000000000002"
        "0000000200000000000102030405060708090a0b0c0d0e0f00000000000000010000000000000002"
        "00000000000000c8e91dbec0a33c606b000000000000000200000000000000c8e91dbec0a33c606b"
        "00000000000000000000000000000000a646354b454e4421",
        "52444254414c303200000002000000000000006000000001000102030405060708090a0b0c0d0e0f"
        "00000000000000040000000000000002000000000000013c6ddee862a646354b0000000000000000"
        "0000000000000000c1721669454e4421",
        "524442435552303200000002000000000000007000000001000102030405060708090a0b0c0d0e0f"
        "00000000000000040000000000000002000000000000013c6ddee862a646354b00000060c1721669"
        "dc7933920000000000000000000000000000000000000000f457761c454e4421",
        "524442434f4d303200000002000000000000007000000000000102030405060708090a0b0c0d0e0f"
        "000000000000000400000060c1721669dc7933920000000000000000000000020000000000000060"
        "00000000dc79339200000000000000000000000000000000bf336709454e4421",
    },
};
const std::string_view nonempty_marker =
    "524442434f4d303200000002000000000000007000000000000102030405060708090a0b0c0d0e0f"
    "000000000000000100000060cc38e707dbfbc8190000000000000000000000010000000000000090"
    "ad924c7404430f7400000000000000000000000000000000c816349e454e4421";

void identities() {
    const auto raw = hex("000102030405060708090a0b0c0d0e0f");
    StoreBytes bytes{};
    std::copy(raw.begin(), raw.end(), bytes.begin());
    StoreId id{bytes};
    check(id.bytes() == bytes && StoreId{bytes} == id, "store byte order/copy");
    fails(Error::InvalidIdentity, [] { (void)StoreId{StoreBytes{}}; });
    fails(Error::InvalidIdentity, [] { (void)GenerationId{std::uint64_t{0}}; });
    check(parse_generation("1").value() == 1, "generation 1");
    check(format_generation(parse_generation("18446744073709551615")) == "18446744073709551615",
          "max generation");
    for (const auto text :
         {"", "0", "00", "01", "+1", "-1", " 1", "1 ", "1/2", "1.snap", "18446744073709551616"})
        fails(Error::InvalidIdentity, [&] { (void)parse_generation(text); });
    fails(Error::InvalidIdentity, [] { (void)parse_generation(std::string_view{"1\0", 2}); });
    fails(Error::GenerationExhausted, [] { (void)next_generation(GenerationId{UINT64_MAX}); });
    fails(Error::SequenceExhausted, [] { (void)next_sequence(LogicalSequence{UINT64_MAX}); });
    check(next_generation(parse_generation("1")).value() == 2, "generation advance");
    check(next_sequence(LogicalSequence{std::uint64_t{0}}).value() == 1, "sequence advance");
    fails(Error::InvalidIdentity, [] { (void)TailOffset{std::uint64_t{95}}; });
    fails(Error::InvalidIdentity, [] { (void)TailOffset{UINT64_MAX}; });
    check(TailOffset{MaxFileBytes}.value() == MaxFileBytes && Crc32c{0U}.value() == 0,
          "offset/CRC domains");
    const auto bytes123 = hex("313233343536373839");
    check(checksum(bytes123).value() == 0xe3069283U && checksum({}).value() == 0, "CRC32C sanity");
    std::vector<std::byte> multi(1024, std::byte{0xff});
    check(checksum(multi).value() == redisdb::persistence::crc32c(std::string(1024, '\xff')),
          "CRC chunk extension");
}
void positives() {
    for (const auto& g : golden) {
        auto b = decode_base_header(hex(g.base));
        auto t = decode_tail_header(hex(g.tail));
        auto s = decode_selector(hex(g.current));
        auto m = decode_marker(hex(g.marker));
        equal(encode_base_header(b), hex(g.base));
        equal(encode_tail_header(t), hex(g.tail));
        equal(encode_selector(s), hex(g.current));
        equal(encode_marker(m), hex(g.marker));
        validate_header_links(b, t, m, s);
        const auto ti = tail_header_identity(t);
        const auto cp = initial_checkpoint(ti);
        check(ti.header_crc.value() == s.tail_header_crc &&
                  ti.full_header_crc.value() == s.tail_full_header_crc,
              "tail CRC binding");
        check(ti.header_crc != ti.full_header_crc, "stored vs full CRC");
        check(cp.sequence.value() == b.sequence && cp.offset.value() == 96 &&
                  cp.record_crc.value() == 0 && cp.prefix_crc == ti.full_header_crc,
              "initial checkpoint S=0/positive");
        check(tail_header_identity(decode_tail_header(encode_tail_header(t))) == ti,
              "identity roundtrip");
        auto copy = cp;
        check(copy == cp, "checkpoint copy/equality");
    }
    const auto t = decode_tail_header(hex(golden[0].tail));
    const auto m = decode_marker(hex(nonempty_marker));
    validate_marker_header(m, t);
    check(m.durable == SourceTuple{1, 144, 0xad924c74U, 0x04430f74U},
          "nonempty marker structural fields");
    auto imported = decode_base_header(hex(golden[0].base));
    imported.sequence = 2;
    imported.key_count = 1;
    imported.logical_charge = 64;
    imported.source.kind = 1;
    imported.source.observed = {2, 120, 123, 456};
    imported.source.durable = {1, 80, 11, 22};
    imported.source.snapshot_generation = 5;
    check(decode_base_header(encode_base_header(imported)) == imported,
          "V1 import observed/durable/snapshot");
    imported.source.durable = imported.source.observed;
    check(decode_base_header(encode_base_header(imported)) == imported, "V1 synchronized source");
    // V1 empty source prefix, calculated with the independently frozen V1 bytes.
    auto legacy = hex("524442414f4630310000000100000000");
    const auto crc = checksum(legacy).value();
    for (unsigned i = 4; i != 0; --i)
        legacy.push_back(static_cast<std::byte>((crc >> ((i - 1) * 8)) & 255U));
    imported.key_count = 0;
    imported.logical_charge = 0;
    imported.sequence = 0;
    imported.source.observed = {0, 20, 0, checksum(legacy).value()};
    imported.source.durable = imported.source.observed;
    imported.source.snapshot_generation = 0;
    check(decode_base_header(encode_base_header(imported)) == imported, "empty V1 provenance");
    auto compacted = decode_base_header(hex(golden[1].base));
    compacted.source.observed.offset = 96;
    compacted.source.observed.record_crc = 0;
    compacted.source.durable = compacted.source.observed;
    check(decode_base_header(encode_base_header(compacted)) == compacted,
          "positive-S header-only source");
    compacted.sequence = UINT64_MAX;
    compacted.source.observed.sequence = UINT64_MAX;
    compacted.source.durable = compacted.source.observed;
    check(decode_base_header(encode_base_header(compacted)) == compacted,
          "exhausted sequence remains readable");
}

template <class Decode>
void common_failures(std::string_view value, Decode decode, std::string_view v1, bool base = false,
                     bool marker = false) {
    const auto bytes = hex(value);
    for (std::size_t n = 0; n < bytes.size(); ++n)
        fails(Error::InvalidLength, [&] { (void)decode(ByteView{bytes}.first(n)); });
    auto extra = bytes;
    extra.push_back(std::byte{0});
    fails(Error::InvalidLength, [&] { (void)decode(extra); });
    auto field = [&](std::size_t offset, std::uint64_t n, unsigned width, Error error) {
        auto copy = bytes;
        number(copy, offset, n, width);
        reseal(copy);
        fails(error, [&] { (void)decode(copy); });
    };
    field(0, 0, 1, Error::InvalidMagic);
    auto old = bytes;
    for (std::size_t i = 0; i < 8; ++i)
        old[i] = static_cast<std::byte>(v1[i]);
    reseal(old);
    fails(Error::InvalidMagic, [&] { (void)decode(old); });
    field(8, 1, 4, Error::UnsupportedVersion);
    field(16, bytes.size() + 1, 4, Error::InvalidLength);
    field(12, base ? 2 : 1, 4, Error::UnsupportedFlags);
    if (!marker)
        field(20, 2, 4, Error::InvalidAccounting);
    auto zero = bytes;
    std::fill(zero.begin() + 24, zero.begin() + 40, std::byte{0});
    reseal(zero);
    fails(Error::InvalidIdentity, [&] { (void)decode(zero); });
    field(40, 0, 8, Error::InvalidIdentity);
    auto bad = bytes;
    bad.back() = std::byte{0};
    fails(Error::InvalidTrailer, [&] { (void)decode(bad); });
    bad = bytes;
    bad[bytes.size() - 8] ^= std::byte{1};
    fails(Error::InvalidChecksum, [&] { (void)decode(bad); });
    std::vector<std::pair<std::size_t, std::size_t>> reserved;
    if (base)
        reserved = {{84, 4}, {168, 8}};
    else if (marker)
        reserved = {{20, 4}, {60, 4}, {88, 16}};
    else if (bytes.size() == 96)
        reserved = {{72, 16}};
    else
        reserved = {{84, 20}};
    for (const auto& [offset, size] : reserved)
        for (std::size_t i = 0; i < size; ++i)
            field(offset + i, 1, 1, Error::InvalidReserved);
}
void malformed_headers() {
    common_failures(golden[0].base, decode_base_header, "RDBSNP01", true);
    common_failures(golden[0].tail, decode_tail_header, "RDBAOF01");
    common_failures(golden[0].marker, decode_marker, "RDBACM01", false, true);
    common_failures(golden[0].current, decode_selector, "RDBSEL01");
    auto b = decode_base_header(hex(golden[0].base));
    auto base_fail = [&](auto edit, Error error) {
        auto x = b;
        edit(x);
        fails(error, [&] { (void)encode_base_header(x); });
    };
    base_fail([](auto& x) { x.flags = 2; }, Error::UnsupportedFlags);
    base_fail([](auto& x) { x.accounting_model = 0; }, Error::InvalidAccounting);
    base_fail([](auto& x) { x.source.kind = 3; }, Error::InvalidProvenance);
    base_fail([](auto& x) { x.source.observed.offset = 20; }, Error::InvalidProvenance);
    base_fail([](auto& x) { x.generation = 2; }, Error::InvalidProvenance);
    base_fail([](auto& x) { x.lru_count = 1; }, Error::InvalidCounts);
    base_fail(
        [](auto& x) {
            x.flags = 1;
            x.key_count = 1;
            x.logical_charge = 64;
        },
        Error::InvalidCounts);
    base_fail([](auto& x) { x.key_count = UINT64_MAX; }, Error::InvalidCounts);
    base_fail(
        [](auto& x) {
            x.key_count = 1;
            x.logical_charge = 63;
        },
        Error::InvalidCounts);
    auto binary = hex(golden[0].base);
    number(binary, 80, 3, 4);
    reseal(binary);
    fails(Error::InvalidProvenance, [&] { (void)decode_base_header(binary); });
    b = decode_base_header(hex(golden[1].base));
    for (auto edit : {+[](BaseHeaderFields& x) { x.source.store = {}; },
                      +[](BaseHeaderFields& x) { x.source.generation = x.generation; },
                      +[](BaseHeaderFields& x) { x.source.durable.record_crc ^= 1; },
                      +[](BaseHeaderFields& x) { x.source.observed.sequence = 0; },
                      +[](BaseHeaderFields& x) { x.source.snapshot_generation = 1; },
                      +[](BaseHeaderFields& x) {
                          x.source.observed.offset = 95;
                          x.source.durable = x.source.observed;
                      },
                      +[](BaseHeaderFields& x) {
                          x.source.observed.offset = 96;
                          x.source.observed.record_crc = 1;
                          x.source.durable = x.source.observed;
                      }})
        base_fail(edit, Error::InvalidProvenance);
    b = decode_base_header(hex(golden[0].base));
    b.source.kind = 1;
    b.sequence = 2;
    b.source.observed = {2, 120, 1, 2};
    b.source.durable = {1, 80, 3, 4};
    for (auto edit : {+[](BaseHeaderFields& x) { x.generation = 2; },
                      +[](BaseHeaderFields& x) { x.source.store = x.store; },
                      +[](BaseHeaderFields& x) { x.source.generation = 1; },
                      +[](BaseHeaderFields& x) { x.source.durable.sequence = 3; },
                      +[](BaseHeaderFields& x) { x.source.durable.offset = 120; },
                      +[](BaseHeaderFields& x) { x.source.durable.sequence = 2; },
                      +[](BaseHeaderFields& x) { x.source.observed.offset = 20; },
                      +[](BaseHeaderFields& x) { x.source.durable = {0, 20, 0, 0}; }})
        base_fail(edit, Error::InvalidProvenance);
}
void bindings_and_bad_arguments() {
    auto b = decode_base_header(hex(golden[0].base));
    auto t = decode_tail_header(hex(golden[0].tail));
    auto s = decode_selector(hex(golden[0].current));
    auto m = decode_marker(hex(golden[0].marker));
    // All edited objects are re-encoded AND decoded: own checksums are valid.
    for (auto edit : {+[](TailHeaderFields& x) { x.base.store[0] ^= std::byte{1}; },
                      +[](TailHeaderFields& x) { ++x.base.generation; },
                      +[](TailHeaderFields& x) { ++x.base.sequence; },
                      +[](TailHeaderFields& x) { ++x.base.byte_length; },
                      +[](TailHeaderFields& x) { x.base.file_crc ^= 1; },
                      +[](TailHeaderFields& x) { x.base.header_crc ^= 1; }}) {
        auto x = t;
        edit(x);
        x = decode_tail_header(encode_tail_header(x));
        fails(Error::CrossHeaderMismatch, [&] { validate_header_links(b, x, m, s); });
    }
    for (auto edit : {+[](SelectorFields& x) { x.tail_header_crc ^= 1; },
                      +[](SelectorFields& x) { x.tail_full_header_crc ^= 1; },
                      +[](SelectorFields& x) { ++x.base.generation; },
                      +[](SelectorFields& x) { x.base.file_crc ^= 1; }}) {
        auto x = s;
        edit(x);
        x = decode_selector(encode_selector(x));
        fails(Error::CrossHeaderMismatch, [&] { validate_header_links(b, t, m, x); });
    }
    for (auto edit : {+[](MarkerFields& x) { x.tail.store[0] ^= std::byte{1}; },
                      +[](MarkerFields& x) { ++x.tail.generation; },
                      +[](MarkerFields& x) { x.tail.header_crc ^= 1; },
                      +[](MarkerFields& x) {
                          x.tail.full_header_crc ^= 1;
                          x.durable.prefix_crc ^= 1;
                      }}) {
        auto x = m;
        edit(x);
        x = decode_marker(encode_marker(x));
        fails(Error::CrossHeaderMismatch, [&] { validate_marker_header(x, t); });
    }
    for (auto edit : {+[](MarkerFields& x) { x.durable.offset = 97; },
                      +[](MarkerFields& x) { x.durable.record_crc = 1; },
                      +[](MarkerFields& x) { x.durable.prefix_crc ^= 1; },
                      +[](MarkerFields& x) { x.durable.sequence = 1; }}) {
        auto x = m;
        edit(x);
        // Build checksum-correct bytes to exercise decode plus supplied-header checks.
        auto bytes = encode_marker(m);
        number(bytes, 64, x.durable.sequence, 8);
        number(bytes, 72, x.durable.offset, 8);
        number(bytes, 80, x.durable.record_crc, 4);
        number(bytes, 84, x.durable.prefix_crc, 4);
        reseal(bytes);
        fails(Error::InvalidBoundary, [&] { validate_marker_header(decode_marker(bytes), t); });
        if (x.durable.sequence == 0 || x.durable.record_crc != 0 ||
            x.durable.prefix_crc != m.tail.full_header_crc)
            fails(Error::InvalidBoundary, [&] { (void)encode_marker(x); });
    }
    auto positive = decode_tail_header(hex(golden[1].tail));
    auto pm = decode_marker(hex(golden[1].marker));
    pm.durable.sequence = 0;
    pm = decode_marker(encode_marker(pm));
    fails(Error::InvalidBoundary, [&] { validate_marker_header(pm, positive); });
    for (auto length : {std::uint64_t{0}, std::uint64_t{203}, UINT64_MAX}) {
        auto x = t;
        x.base.byte_length = length;
        fails(Error::InvalidLength, [&] { (void)encode_tail_header(x); });
        auto y = s;
        y.base.byte_length = length;
        fails(Error::InvalidLength, [&] { (void)encode_selector(y); });
    }
    auto ti = tail_header_identity(t);
    ti.header_length = 97;
    fails(Error::CrossHeaderMismatch, [&] { (void)initial_checkpoint(ti); });
    auto offset = m;
    offset.durable.offset = 95;
    fails(Error::InvalidIdentity, [&] { (void)encode_marker(offset); });
    offset.durable.offset = UINT64_MAX;
    fails(Error::InvalidIdentity, [&] { (void)encode_marker(offset); });
    auto length = m;
    length.tail.header_length = 97;
    fails(Error::InvalidLength, [&] { (void)encode_marker(length); });
    auto ss = s;
    ss.tail_header_length = 97;
    fails(Error::InvalidLength, [&] { (void)encode_selector(ss); });
    // Shared wrong base CRC claims cannot be verified without the complete base.
    auto tt = t;
    auto sm = s;
    tt.base.file_crc ^= 1;
    sm.base = tt.base;
    auto id = tail_header_identity(tt);
    sm.tail_header_crc = id.header_crc.value();
    sm.tail_full_header_crc = id.full_header_crc.value();
    auto mm = m;
    mm.tail.header_crc = id.header_crc.value();
    mm.tail.full_header_crc = id.full_header_crc.value();
    mm.durable.prefix_crc = id.full_header_crc.value();
    validate_header_links(b, tt, mm,
                          sm); // consistency only, deliberately not full-file validation
}
void raw_field_failures() {
    auto reject = [](std::string_view golden_hex, auto decode, std::size_t offset,
                     std::uint64_t value, unsigned width, Error error) {
        auto bytes = hex(golden_hex);
        number(bytes, offset, value, width);
        reseal(bytes);
        fails(error, [&] { (void)decode(bytes); });
    };
    for (auto size : {std::uint64_t{0}, std::uint64_t{203}, UINT64_MAX}) {
        reject(golden[0].tail, decode_tail_header, 56, size, 8, Error::InvalidLength);
        reject(golden[0].current, decode_selector, 56, size, 8, Error::InvalidLength);
    }
    for (auto offset : {std::uint64_t{0}, std::uint64_t{95}, UINT64_MAX})
        reject(golden[0].marker, decode_marker, 72, offset, 8, Error::InvalidIdentity);
    reject(golden[0].marker, decode_marker, 48, 95, 4, Error::InvalidLength);
    reject(golden[0].current, decode_selector, 72, 95, 4, Error::InvalidLength);
    reject(golden[0].base, decode_base_header, 72, 1, 8, Error::InvalidCounts);
    reject(golden[0].base, decode_base_header, 56, UINT64_MAX, 8, Error::InvalidCounts);
    reject(golden[1].base, decode_base_header, 88, 0, 8, Error::InvalidProvenance);
    reject(golden[1].base, decode_base_header, 104, 3, 8, Error::InvalidProvenance);
    reject(golden[1].base, decode_base_header, 136, 2, 8, Error::InvalidProvenance);
    auto t = decode_tail_header(hex(golden[0].tail));
    t.base.generation = 0x0102030405060708ULL;
    t.base.sequence = 0xf1f2f3f4f5f6f7f8ULL;
    t.base.byte_length = 0x0102030405ULL;
    t.base.file_crc = 0xa1b2c3d4U;
    const auto bytes = encode_tail_header(t);
    equal(ByteView{bytes}.subspan(40, 8), hex("0102030405060708"));
    equal(ByteView{bytes}.subspan(48, 8), hex("f1f2f3f4f5f6f7f8"));
    equal(ByteView{bytes}.subspan(56, 8), hex("0000000102030405"));
    equal(ByteView{bytes}.subspan(64, 4), hex("a1b2c3d4"));
    check(decode_tail_header(bytes) == t, "high-bit big-endian roundtrip");
    auto ti = tail_header_identity(t);
    ti.full_header_crc = Crc32c{0U};
    fails(Error::CrossHeaderMismatch, [&] { (void)initial_checkpoint(ti); });
}

} // namespace
int main() {
    try {
        identities();
        positives();
        malformed_headers();
        bindings_and_bad_arguments();
        raw_field_failures();
        std::cout << "PASS V2 fixed metadata: canonical E/C/L/T portions, identities/provenance, "
                     "malformed headers and bindings; no streaming or filesystem validation\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
