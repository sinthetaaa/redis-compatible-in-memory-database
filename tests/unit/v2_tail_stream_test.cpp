#include "redisdb/persistence/V2TailStream.hpp"

#include "redisdb/persistence/AofLog.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace {
long allocation_budget = -1;
std::int64_t live_allocations = 0;
bool measure = false;
std::size_t allocated_bytes = 0, allocation_calls = 0, largest_allocation = 0;
} // namespace
void* operator new(std::size_t n) {
    if (allocation_budget == 0)
        throw std::bad_alloc{};
    if (allocation_budget > 0)
        --allocation_budget;
    if (void* p = std::malloc(n ? n : 1)) {
        ++live_allocations;
        if (measure) {
            allocated_bytes += n;
            ++allocation_calls;
            largest_allocation = std::max(largest_allocation, n);
        }
        return p;
    }
    throw std::bad_alloc{};
}
// Keep deallocation hooks out of optimizer inlining, which otherwise produces
// GCC mismatched-new-delete warnings for this deliberate malloc/free new seam.
[[gnu::noinline]] void operator delete(void* p) noexcept {
    if (p)
        --live_allocations;
    std::free(p);
}
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
[[gnu::noinline]] void operator delete[](void* p) noexcept { ::operator delete(p); }
[[gnu::noinline]] void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace redisdb;
using namespace persistence::v2;
static_assert(!std::is_default_constructible_v<ValidatedTailScan>);
static_assert(!std::is_constructible_v<ValidatedTailScan, MarkerFields>);
void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
ByteView view(std::string_view s) { return std::as_bytes(std::span{s.data(), s.size()}); }
std::uint32_t reverse(std::uint32_t x) {
    std::uint32_t r = 0;
    for (unsigned i = 0; i < 32; ++i) {
        r = (r << 1) | (x & 1U);
        x >>= 1;
    }
    return r;
}
// Independent forward-polynomial Castagnoli reference, reflected bytes/register.
// Table form permits deterministic exact-limit stress in Debug and UBSan.
std::uint32_t crc(std::string_view s, std::uint32_t previous = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto r = i << 24;
            for (unsigned j = 0; j < 8; ++j)
                r = (r << 1) ^ ((r & 0x80000000U) ? 0x1edc6f41U : 0U);
            t[i] = r;
        }
        return t;
    }();
    static const auto reflected = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i)
            t[i] = reverse(i) >> 24;
        return t;
    }();
    auto r = reverse(~previous);
    for (char c : s)
        r = (r << 8) ^ table[(r >> 24) ^ reflected[static_cast<unsigned char>(c)]];
    return ~reverse(r);
}
std::string be(std::uint64_t n, unsigned width) {
    std::string s;
    for (unsigned i = width; i != 0; --i)
        s.push_back(static_cast<char>((n >> ((i - 1) * 8)) & 255U));
    return s;
}
void put(std::string& s, std::size_t at, std::uint64_t n, unsigned width) {
    s.replace(at, width, be(n, width));
}
std::uint32_t u32(std::string_view s, std::size_t at) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i)
        n = (n << 8) | static_cast<unsigned char>(s[at + i]);
    return n;
}
std::string blob(std::string_view s) { return be(s.size(), 4) + std::string{s}; }
std::string sealed(std::string prefix) {
    prefix += be(crc(prefix), 4);
    prefix += "END!";
    return prefix;
}
std::string header(std::uint64_t g = 1, std::uint64_t seq = 0, std::uint64_t base_size = 204,
                   std::uint32_t base_crc = 0x4ebad660U, std::uint32_t base_hcrc = 0xbc252022U) {
    std::string s(88, '\0');
    s.replace(0, 8, "RDBTAL02");
    put(s, 8, 2, 4);
    put(s, 16, 96, 4);
    put(s, 20, 1, 4);
    for (unsigned i = 0; i < 16; ++i)
        s[24 + i] = static_cast<char>(i);
    put(s, 40, g, 8);
    put(s, 48, seq, 8);
    put(s, 56, base_size, 8);
    put(s, 64, base_crc, 4);
    put(s, 68, base_hcrc, 4);
    return sealed(s);
}
std::string effect(std::string_view key, unsigned tag, std::string value = {},
                   std::optional<std::int64_t> deadline = {}) {
    auto s = blob(key) + be(tag, 1);
    if (tag) {
        s += be(deadline.has_value(), 1);
        if (deadline)
            s += be(std::bit_cast<std::uint64_t>(*deadline), 8);
        s += value;
    }
    return s;
}
std::string effects(const std::vector<std::string>& entries, bool lru = false,
                    const std::vector<std::string>& order = {}) {
    auto s = be(entries.size(), 4);
    for (const auto& e : entries)
        s += e;
    s += be(lru, 1) + be(order.size(), 4);
    for (const auto& k : order)
        s += blob(k);
    return s;
}
std::string string_effects(std::string_view key, std::string_view value) {
    return effects({effect(key, 1, blob(value))});
}
std::string txn(std::uint64_t seq, std::string_view payload) {
    return sealed("TXN1" + be(seq, 8) + be(payload.size(), 4) + std::string{payload});
}
MarkerFields marker(std::string_view h, SourceTuple tuple) {
    // Independent exact marker bytes, then accepted fixed decoder supplies the
    // structurally decoded input expected by the new validator.
    std::string s(104, '\0');
    s.replace(0, 8, "RDBCOM02");
    put(s, 8, 2, 4);
    put(s, 16, 112, 4);
    s.replace(24, 24, h.substr(24, 24));
    put(s, 48, 96, 4);
    put(s, 52, u32(h, 88), 4);
    put(s, 56, crc(h), 4);
    put(s, 64, tuple.sequence, 8);
    put(s, 72, tuple.offset, 8);
    put(s, 80, tuple.record_crc, 4);
    put(s, 84, tuple.prefix_crc, 4);
    return decode_marker(view(sealed(s)));
}
MarkerFields resealed_marker(MarkerFields m) {
    // Structurally valid hostile tuples retain correct own fixed checksums.
    return decode_marker(encode_marker(m));
}
struct Fixture {
    TailHeaderFields expected;
    MarkerFields claim;
    std::string bytes;
    SourceTuple observed;
    std::uint64_t records;
};
Fixture fixture(std::string h, const std::vector<std::string>& records = {},
                std::size_t boundary = SIZE_MAX) {
    const auto expected = decode_tail_header(view(h));
    SourceTuple observed{expected.base.sequence, 96, 0, crc(h)};
    auto matched = observed;
    if (boundary == SIZE_MAX)
        boundary = records.size();
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        std::uint64_t seq = 0;
        for (std::size_t j = 4; j < 12; ++j)
            seq = (seq << 8) | static_cast<unsigned char>(r[j]);
        observed = {seq, h.size() + r.size(), u32(r, r.size() - 8), crc(r, observed.prefix_crc)};
        h += r;
        if (boundary == i + 1)
            matched = observed;
    }
    return {expected, marker(std::string_view{h}.substr(0, 96), matched), std::move(h), observed,
            records.size()};
}
std::size_t positives = 0, partitions = 0, negatives = 0, truncations = 0, resource_cases = 0;
void inspect(const TailStreamValidator& v, const Fixture& f) {
    check(v.status() == StreamStatus::Complete && v.result(), "no final scan");
    const auto& r = *v.result();
    const auto id = tail_header_identity(f.expected);
    auto cp = [&](const SourceTuple& t) {
        return GenerationCheckpoint{id, LogicalSequence{t.sequence}, TailOffset{t.offset},
                                    Crc32c{t.record_crc}, Crc32c{t.prefix_crc}};
    };
    check(r.header() == f.expected && r.identity() == id && r.observed() == cp(f.observed) &&
              r.matched_boundary() == cp(f.claim.durable) && r.byte_length() == f.bytes.size() &&
              r.record_count() == f.records && v.bytes_received() == f.bytes.size() &&
              v.buffered_bytes() == 0 && v.buffer_capacity() <= MaxTailRecordBytes,
          "observed/matched checkpoint differs");
}
void accept(const Fixture& f, std::size_t chunk) {
    TailStreamValidator v(f.expected, f.claim, f.bytes.size());
    check(v.feed({}) == StreamStatus::NeedMore, "empty feed");
    for (std::size_t at = 0; at < f.bytes.size();) {
        const auto n = std::min(chunk, f.bytes.size() - at);
        check(v.feed(view(std::string_view{f.bytes}.substr(at, n))) == StreamStatus::NeedMore &&
                  !v.result(),
              "early result or rejected valid data");
        at += n;
    }
    check(v.feed({}) == StreamStatus::NeedMore && !v.result(), "EOF assumed from byte extent");
    check(v.finish() == StreamStatus::Complete, "valid finish failed");
    inspect(v, f);
    check(v.finish() == StreamStatus::Complete, "repeated finish");
    check(v.feed({}) == StreamStatus::Error && !v.result() &&
              v.error() == TailStreamError::Finalized && v.finish() == StreamStatus::Error,
          "feed after finalization");
    ++positives;
}
void reject(Fixture f, std::optional<TailStreamError> category = {},
            std::optional<std::uint64_t> extent = {}) {
    TailStreamValidator v(f.expected, f.claim, extent.value_or(f.bytes.size()));
    (void)v.feed(view(f.bytes));
    check(v.finish() == StreamStatus::Error && !v.result(), "invalid tail accepted");
    if (category && v.error() != *category) {
        std::cerr << "negative " << negatives << " expected " << static_cast<int>(*category)
                  << " got " << static_cast<int>(v.error()) << '\n';
        check(false, "wrong tail error category");
    }
    const auto e = v.error();
    check(v.feed({}) == StreamStatus::Error && v.feed(view("extra")) == StreamStatus::Error &&
              v.finish() == StreamStatus::Error && v.error() == e && !v.result(),
          "failure not terminal");
    ++negatives;
}

void golden_and_partitions() {
    check(crc("123456789") == 0xe3069283U, "independent CRC reference");
    const auto h = header();
    const auto t = txn(1, string_effects(std::string("\0k", 2), std::string("\xff\0v", 3)));
    const auto u = txn(2, effects({effect(std::string(1, '\xff'), 1, blob("7"))}, true,
                                  {std::string(1, '\xff'), std::string("\0k", 2)}));
    const auto c = header(3, 1, 244, 0x155d9af1U, 0x213707daU);
    const auto l = header(4, 2, 316, 0x6ddee862U, 0xa646354bU);
    check(h.size() == 96 && crc(h) == 0xdbfbc819U && u32(h, 88) == 0xcc38e707U && t.size() == 48 &&
              u32(t, t.size() - 8) == 0xad924c74U && crc(t) == 0xb5c6c2b9U &&
              crc(h + t) == 0x04430f74U && u.size() == 56 && u32(u, u.size() - 8) == 0xe91dbec0U &&
              crc(u) == 0xf18ecbb6U && crc(h + t + u) == 0xa33c606bU && c.size() == 96 &&
              crc(c) == 0x28b640b9U && u32(c, 88) == 0xad63e9bbU && l.size() == 96 &&
              crc(l) == 0xdc793392U && u32(l, 88) == 0xc1721669U,
          "frozen section-15 E/T/U/C/L vectors");
    const auto tf = fixture(h, {t});
    check(tf.claim.durable == SourceTuple{1, 144, 0xad924c74U, 0x04430f74U}, "frozen T marker");
    check(fixture(h, {t, u}).claim.durable == SourceTuple{2, 200, 0xe91dbec0U, 0xa33c606bU},
          "frozen U boundary");
    const auto tm = encode_marker(tf.claim);
    check(checksum(tm).value() == 0x7a05287bU, "frozen complete T.marker CRC");
    for (const auto& f : {fixture(h), tf, fixture(h, {t, u}, 0), fixture(h, {t, u}, 1),
                          fixture(h, {t, u}, 2), fixture(c), fixture(l)}) {
        accept(f, f.bytes.size());
        accept(f, 1);
        // Every possible two-chunk split of every frozen header/tail example.
        for (std::size_t split = 0; split <= f.bytes.size(); ++split) {
            TailStreamValidator v(f.expected, f.claim, f.bytes.size());
            check(v.feed(view(std::string_view{f.bytes}.substr(0, split))) ==
                          StreamStatus::NeedMore &&
                      v.feed(view(std::string_view{f.bytes}.substr(split))) ==
                          StreamStatus::NeedMore &&
                      !v.result() && v.finish() == StreamStatus::Complete,
                  "two-chunk partition");
            inspect(v, f);
            ++partitions;
        }
        TailStreamValidator irregular(f.expected, f.claim, f.bytes.size());
        std::size_t at = 0, step = 1;
        while (at < f.bytes.size()) {
            const auto n = std::min(step, f.bytes.size() - at);
            auto owned = f.bytes.substr(at, n);
            check(irregular.feed(view(owned)) == StreamStatus::NeedMore, "irregular feed");
            std::fill(owned.begin(), owned.end(), '?');
            at += n;
            step = (step * 17) % 41 + 1;
        }
        check(irregular.finish() == StreamStatus::Complete, "chunk bytes were borrowed");
        inspect(irregular, f);
        ++partitions;
        for (std::size_t n = 0; n < f.bytes.size(); ++n) {
            auto short_f = f;
            short_f.bytes.resize(n);
            reject(std::move(short_f), {}, f.bytes.size());
            ++truncations;
        }
    }
}

std::string typed_effects() {
    return effects(
        {effect("", 1, blob("")), effect(std::string(1, '\0'), 1, blob(std::string("\0\xff", 2))),
         effect("gone", 0), effect("h", 3, be(2, 4) + blob("z") + blob("v") + blob("a") + blob("")),
         effect("l", 2, be(3, 4) + blob("") + blob("x") + blob("x")),
         effect("n", 1, blob("0007"), -1), effect("s", 4, be(2, 4) + blob("z") + blob("a")),
         effect(std::string(1, '\x80'), 1, blob(""), 1),
         effect(std::string(1, '\xff'), 1, blob("9"), 123456789)},
        true, {"foreign-to-this-record", std::string(1, '\0')});
}
void positive_semantics() {
    const auto p = typed_effects();
    const auto decoded = persistence::decode_effects(p);
    check(decoded.entries.size() == 9 && !decoded.entries[2].replacement && decoded.has_lru &&
              std::get<storage::StringValue>(decoded.entries[5].replacement->value).bytes ==
                  "0007" &&
              std::get<storage::ListValue>(decoded.entries[4].replacement->value).elements ==
                  std::list<std::string>({"", "x", "x"}),
          "independent typed fixture");
    // Journal LRU uniqueness does NOT require a permutation of the changed keys.
    accept(fixture(header(), {txn(1, p)}), 1);
    accept(fixture(header(), {txn(1, effects({effect("last", 0)}, true))}), 1);
    accept(fixture(header(), {txn(1, effects({effect("last", 0)}))}), 1);
    using Duration = storage::WallTimePoint::duration;
    constexpr auto scale = Duration::period::den / 1'000'000;
    for (auto d : {INT64_MIN / scale, INT64_MAX / scale})
        accept(fixture(header(), {txn(1, effects({effect("a", 1, blob(""), d)}))}), 1);
    accept(fixture(header(9, UINT64_MAX)), 1);
    const auto psmall = string_effects("a", "v");
    accept(fixture(header(7, UINT64_MAX - 2),
                   {txn(UINT64_MAX - 1, psmall), txn(UINT64_MAX, psmall)}, 1),
           1);
    // A copied scan owns all metadata even after validator and input destruction.
    auto owned = [&] {
        auto f = fixture(header(8, 17), {txn(18, p)});
        TailStreamValidator v(f.expected, f.claim, f.bytes.size());
        check(v.feed(view(f.bytes)) == StreamStatus::NeedMore, "metadata owner feed");
        std::fill(f.bytes.begin(), f.bytes.end(), '?');
        check(v.finish() == StreamStatus::Complete, "metadata owner finish");
        return *v.result();
    }();
    check(owned.observed().sequence.value() == 18 && owned.record_count() == 1 &&
              owned.identity().base.generation.value() == 8,
          "result borrowed metadata");
    auto f = fixture(header());
    TailStreamValidator v(f.expected, f.claim, f.bytes.size());
    (void)v.feed(view(f.bytes));
    (void)v.finish();
    check(v.feed(view("x")) == StreamStatus::Error && !v.result(), "nonempty feed after finish");
}

void metadata_negatives() {
    const auto h = header();
    const auto good = fixture(h, {txn(1, string_effects("a", "v"))});
    for (auto at : {0U, 8U, 12U, 16U, 20U, 72U, 87U, 88U, 92U}) {
        auto f = good;
        f.bytes[at] ^= 1;
        if (at != 88 && at != 92)
            f.bytes.replace(88, 8, sealed(f.bytes.substr(0, 88)).substr(88));
        reject(f, TailStreamError::Header);
    }
    auto f = good;
    f.bytes.replace(0, 8, "RDBAOF01");
    f.bytes.replace(88, 8, sealed(f.bytes.substr(0, 88)).substr(88));
    reject(f, TailStreamError::Header);
    for (auto at : {24U, 40U, 48U, 56U, 64U, 68U}) {
        f = good;
        f.bytes[at] ^= 1;
        f.bytes.replace(88, 8, sealed(f.bytes.substr(0, 88)).substr(88));
        reject(f, TailStreamError::Identity);
    }
    f = fixture(h);
    f.bytes += '?'; // overlong header becomes an incomplete record, never an extension
    reject(f, TailStreamError::Truncated);
    reject(f, TailStreamError::Length, 96);
    for (auto extent : {std::uint64_t{0}, std::uint64_t{95}, UINT64_MAX})
        reject(good, TailStreamError::Length, extent);
    for (auto edit : {+[](TailHeaderFields& x) { x.base.store = {}; },
                      +[](TailHeaderFields& x) { x.base.generation = 0; },
                      +[](TailHeaderFields& x) { x.base.byte_length = 203; },
                      +[](TailHeaderFields& x) { x.base.accounting_model = 2; }}) {
        f = good;
        edit(f.expected);
        reject(f, TailStreamError::Identity);
    }
    for (auto edit : {+[](MarkerFields& x) { x.tail.store[0] ^= std::byte{1}; },
                      +[](MarkerFields& x) { ++x.tail.generation; },
                      +[](MarkerFields& x) { x.tail.header_crc ^= 1; },
                      +[](MarkerFields& x) { x.tail.full_header_crc ^= 1; }}) {
        f = good;
        edit(f.claim);
        f.claim = resealed_marker(f.claim);
        reject(f, TailStreamError::Marker);
    }
    for (auto edit : {+[](MarkerFields& x) { x.tail.header_length = 97; },
                      +[](MarkerFields& x) { x.tail.store = {}; },
                      +[](MarkerFields& x) { x.durable.offset = 95; },
                      +[](MarkerFields& x) { x.durable.offset = UINT64_MAX; }}) {
        f = good;
        edit(f.claim); // Constructor must also reject caller-assembled invalid fields.
        reject(f, TailStreamError::Marker);
    }
    for (auto edit : {+[](MarkerFields& x) { ++x.durable.offset; },
                      +[](MarkerFields& x) { x.durable.record_crc = 1; },
                      +[](MarkerFields& x) { x.durable.prefix_crc ^= 1; }}) {
        f = fixture(h);
        edit(f.claim);
        reject(f, TailStreamError::Marker);
    }
    f = fixture(header(3, 10));
    --f.claim.durable.sequence;
    reject(f, TailStreamError::Marker);
}

void frame_and_sequence_negatives() {
    const auto p = string_effects("a", "v");
    const auto good = fixture(header(), {txn(1, p)});
    for (auto at : {96U, 112U, static_cast<unsigned>(good.bytes.size() - 8),
                    static_cast<unsigned>(good.bytes.size() - 1)}) {
        auto f = good;
        f.bytes[at] ^= 1;
        reject(f, at == 96                      ? TailStreamError::Frame
                  : at == good.bytes.size() - 1 ? TailStreamError::Trailer
                                                : TailStreamError::Checksum);
    }
    for (auto n : {0U, 1000U, static_cast<unsigned>(MaxTailPayload + 1), UINT32_MAX}) {
        auto f = good;
        put(f.bytes, 108, n, 4);
        reject(f, n == 0 ? TailStreamError::Checksum : TailStreamError::Length);
    }
    for (std::uint64_t seq : {std::uint64_t{0}, std::uint64_t{2}, UINT64_MAX}) {
        auto f = fixture(header(), {txn(seq, p)}, 0);
        reject(f, TailStreamError::Sequence);
    }
    for (std::uint64_t second : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{3}})
        reject(fixture(header(), {txn(1, p), txn(second, p)}, 1), TailStreamError::Sequence);
    reject(fixture(header(2, 10), {txn(10, p)}, 0), TailStreamError::Sequence);
    reject(fixture(header(2, 1), {txn(2, p), txn(1, p)}, 0), TailStreamError::Sequence);
    reject(fixture(header(2, UINT64_MAX), {txn(0, p)}, 0), TailStreamError::Sequence);
    reject(fixture(header(2, UINT64_MAX - 1), {txn(UINT64_MAX, p), txn(0, p)}, 1),
           TailStreamError::Sequence);
    for (const auto& suffix :
         {std::string{"garbagegarbagegarbage"}, std::string{"T"}, std::string{"TXN1"},
          txn(2, p).substr(0, 18), txn(2, p).substr(0, txn(2, p).size() - 1)}) {
        auto f = good;
        f.bytes += suffix;
        reject(f);
    }
    auto shorter_extent = fixture(header(), {txn(1, p)}, 0);
    reject(shorter_extent, TailStreamError::Length, good.bytes.size() - 1);
    reject(good, {}, good.bytes.size() + 1);
}

void semantic_negatives() {
    const auto p = string_effects("a", "v");
    std::vector<std::string> bad{
        effects({}),
        be(1'048'577, 4) + std::string(32, '\0'),
        effects({effect("a", 5, blob(""))}),
        effects({effect("b", 0), effect("a", 0)}),
        effects({effect("a", 0), effect("a", 0)}),
        effects({effect("a", 2, be(0, 4))}),
        effects({effect("a", 3, be(0, 4))}),
        effects({effect("a", 4, be(0, 4))}),
        effects({effect("a", 2, be(UINT32_MAX, 4))}),
        effects({effect("a", 3, be(100, 4) + blob(""))}),
        effects({effect("a", 3, be(2, 4) + blob("z") + blob("1") + blob("z") + blob("2"))}),
        effects({effect("a", 4, be(2, 4) + blob("z") + blob("z"))}),
        effects({effect("a", 1, be(8'388'609, 4))}),
        effects({effect("a", 0)}, false, {"a"}),
        effects({effect("a", 0)}, true, {"x", "x"}),
        p + "suffix"};
    auto timed = p;
    timed[10] = 2;
    bad.push_back(timed);
    auto lru = p;
    lru[lru.size() - 5] = 2;
    bad.push_back(lru);
    auto key = p;
    put(key, 4, UINT32_MAX, 4);
    bad.push_back(key);
    auto order_count = p;
    order_count[order_count.size() - 5] = 1;
    put(order_count, order_count.size() - 4, 1'048'577, 4);
    bad.push_back(order_count);
    using Duration = storage::WallTimePoint::duration;
    constexpr auto scale = Duration::period::den / 1'000'000;
    if constexpr (scale > 1) {
        bad.push_back(effects({effect("a", 1, blob(""), INT64_MIN)}));
        bad.push_back(effects({effect("a", 1, blob(""), INT64_MAX)}));
    }
    for (const auto& value : bad) {
        // Real frame/prefix CRCs are rebuilt; initial marker matches before the
        // corrupt suffix, so semantic validation cannot be bypassed at watermark.
        reject(fixture(header(), {txn(1, value)}, 0), TailStreamError::Semantic);
    }
}

void boundary_negatives() {
    const auto p = string_effects("a", "v");
    const auto good = fixture(header(), {txn(1, p), txn(2, p)}, 1);
    for (auto delta : {-1, 1}) {
        auto f = good;
        f.claim.durable.offset =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(f.claim.durable.offset) + delta);
        f.claim = resealed_marker(f.claim);
        reject(f, TailStreamError::Boundary);
    }
    for (auto edit : {+[](MarkerFields& x) { x.durable.record_crc ^= 1; },
                      +[](MarkerFields& x) { x.durable.prefix_crc ^= 1; },
                      +[](MarkerFields& x) { x.durable.sequence = 3; }}) {
        auto f = good;
        edit(f.claim);
        f.claim = resealed_marker(f.claim);
        reject(f, TailStreamError::Boundary);
    }
    // Every interior byte of a genuine record: even the CRC of precisely that
    // byte prefix cannot make it a transaction boundary. Includes header/payload/
    // checksum/END!, not merely plausible final lengths.
    for (std::size_t at = 97; at < good.claim.durable.offset; ++at) {
        auto f = good;
        f.claim.durable.offset = at;
        f.claim.durable.prefix_crc = crc(std::string_view{f.bytes}.substr(0, at));
        f.claim = resealed_marker(f.claim);
        reject(f, TailStreamError::Boundary);
    }
    auto beyond = good;
    beyond.claim.durable.offset = beyond.bytes.size() + 1;
    beyond.claim = resealed_marker(beyond.claim);
    reject(beyond, TailStreamError::Boundary);
    auto wrong_record = good;
    wrong_record.claim.durable.sequence = 2; // D is valid, O refers to the first record.
    wrong_record.claim = resealed_marker(wrong_record.claim);
    reject(wrong_record, TailStreamError::Boundary);
    auto corrupt_suffix = good;
    corrupt_suffix.bytes.back() = '?';
    reject(corrupt_suffix, TailStreamError::Trailer);
    auto partial_suffix = good;
    partial_suffix.bytes.pop_back();
    reject(partial_suffix, TailStreamError::Truncated, good.bytes.size());
    auto corrupt_crc_suffix = good;
    corrupt_crc_suffix.bytes[corrupt_crc_suffix.bytes.size() - 8] ^= 1;
    reject(corrupt_crc_suffix, TailStreamError::Checksum);
    // A matched header boundary also cannot hide later framing corruption.
    auto initial = fixture(header(), {txn(1, p)}, 0);
    initial.bytes.back() = '?';
    reject(initial, TailStreamError::Trailer);
    // Stop after a complete record while declaring a missing complete later one.
    auto missing = good;
    missing.bytes.resize(good.claim.durable.offset);
    reject(missing, TailStreamError::Length, good.bytes.size());
}

void resource_tests() {
    const auto f =
        fixture(header(), {txn(1, typed_effects()), txn(2, string_effects("a", "v"))}, 1);
    for (const auto limits : {TailStreamLimits{1, UINT64_MAX}, TailStreamLimits{MaxTailPayload, 0},
                              TailStreamLimits{MaxTailPayload, 1}}) {
        TailStreamValidator v(f.expected, f.claim, f.bytes.size(), limits);
        check(v.feed(view(f.bytes)) == StreamStatus::Error &&
                  v.error() == TailStreamError::ResourceLimit && !v.result(),
              "resource budget");
        ++resource_cases;
    }
    TailStreamValidator valid(f.expected, f.claim, f.bytes.size(), {MaxTailPayload, 2});
    check(valid.feed(view(f.bytes)) == StreamStatus::NeedMore &&
              valid.finish() == StreamStatus::Complete,
          "valid resource budget");
    inspect(valid, f);
    ++resource_cases;
    auto empty = fixture(header());
    TailStreamValidator zero(empty.expected, empty.claim, 96, {0, 0});
    check(zero.feed(view(empty.bytes)) == StreamStatus::NeedMore &&
              zero.finish() == StreamStatus::Complete,
          "zero budgets with no records");
    ++resource_cases;
    // Force successive buffer growth after the first genuine marker match,
    // exercising failures during both replacement allocation and later decoding.
    const auto allocation_f = fixture(header(),
                                      {txn(1, string_effects("a", "v")), txn(2, typed_effects()),
                                       txn(3, string_effects("a", std::string(8192, 'x')))},
                                      1);
    bool succeeded = false;
    std::size_t failures = 0;
    for (long at = 0; at < 1000; ++at) {
        allocation_budget = at;
        TailStreamValidator v(allocation_f.expected, allocation_f.claim, allocation_f.bytes.size());
        const auto fed = v.feed(view(allocation_f.bytes));
        const auto final = v.finish();
        allocation_budget = -1;
        if (fed == StreamStatus::Error) {
            check(final == StreamStatus::Error && v.error() == TailStreamError::ResourceLimit &&
                      !v.result() && v.feed({}) == StreamStatus::Error,
                  "allocation failure state");
            ++failures;
        } else {
            check(final == StreamStatus::Complete, "allocation campaign finalization");
            inspect(v, allocation_f);
            succeeded = true;
            break;
        }
    }
    check(succeeded && failures > 20, "allocation campaign incomplete");
    // Hostile length claims fail before allocating the claimed payload, even
    // when the caller's untrusted declared file extent is INT64_MAX.
    for (auto n : {MaxTailPayload + 1, static_cast<std::size_t>(UINT32_MAX)}) {
        auto prelude = header() + "TXN1" + be(1, 8) + be(n, 4);
        TailStreamValidator v(empty.expected, empty.claim, MaxFileBytes);
        largest_allocation = 0;
        measure = true;
        const auto status = v.feed(view(prelude));
        measure = false;
        check(status == StreamStatus::Error && v.error() == TailStreamError::Length &&
                  largest_allocation <= 96,
              "oversized length allocated");
        ++resource_cases;
    }
    auto capped = header() + "TXN1" + be(1, 8) + be(MaxTailPayload, 4);
    TailStreamValidator tiny(empty.expected, empty.claim, capped.size());
    check(tiny.feed(view(capped)) == StreamStatus::Error && tiny.error() == TailStreamError::Length,
          "length impossible within extent");
    ++resource_cases;
    TailStreamValidator huge(empty.expected, empty.claim, MaxFileBytes);
    check(huge.feed(view(empty.bytes)) == StreamStatus::NeedMore && huge.buffer_capacity() == 96 &&
              huge.finish() == StreamStatus::Error,
          "declared extent caused aggregate allocation");
    ++resource_cases;
    const auto medium = fixture(header(), {txn(1, string_effects("a", std::string(8192, 'x')))});
    std::array<std::size_t, 2> bytes{}, calls{};
    for (std::size_t trial = 0; trial < 2; ++trial) {
        TailStreamValidator v(medium.expected, medium.claim, medium.bytes.size());
        allocated_bytes = allocation_calls = 0;
        measure = true;
        const auto chunk = trial == 0 ? medium.bytes.size() : 1;
        for (std::size_t at = 0; at < medium.bytes.size();) {
            auto n = std::min(chunk, medium.bytes.size() - at);
            (void)v.feed(view(std::string_view{medium.bytes}.substr(at, n)));
            at += n;
        }
        (void)v.finish();
        measure = false;
        inspect(v, medium);
        bytes[trial] = allocated_bytes;
        calls[trial] = allocation_calls;
    }
    check(bytes[0] == bytes[1] && calls[0] == calls[1], "chunk-count-dependent allocation");
    std::cout << "allocation failures: " << failures << "; whole/bytewise allocation bytes "
              << bytes[0] << '/' << bytes[1] << ", calls " << calls[0] << '/' << calls[1] << '\n';
}

void maximum_record_and_history() {
    std::string value = be(8, 4);
    value.reserve(MaxTailPayload);
    for (unsigned i = 0; i < 8; ++i)
        value += blob(std::string(8'388'608 - (i == 7 ? 51 : 0), 'x'));
    auto p = effects({effect("", 2, std::move(value))});
    check(p.size() == MaxTailPayload, "exact-limit payload fixture");
    const auto large = txn(1, p);
    std::string{}.swap(p);
    check(large.size() == MaxTailRecordBytes, "exact-limit record fixture");
    const auto h = header();
    const auto small_payload = effects({effect("a", 0)});
    auto small = txn(2, small_payload);
    constexpr std::uint64_t extra = 4096;
    const auto extent = h.size() + large.size() + extra * small.size();
    auto prefix = crc(large, crc(h));
    std::uint32_t last_crc = 0;
    for (std::uint64_t seq = 2; seq <= extra + 1; ++seq) {
        small = txn(seq, small_payload);
        prefix = crc(small, prefix);
        last_crc = u32(small, small.size() - 8);
    }
    const auto expected = decode_tail_header(view(h));
    TailStreamValidator v(expected, marker(h, {0, 96, 0, crc(h)}), extent);
    check(v.feed(view(h)) == StreamStatus::NeedMore, "large tail header");
    largest_allocation = 0;
    measure = true;
    for (std::size_t at = 0; at < large.size();) {
        const auto n = std::min<std::size_t>(4093, large.size() - at);
        check(v.feed(view(std::string_view{large}.substr(at, n))) == StreamStatus::NeedMore,
              "large record feed");
        check(v.buffer_capacity() <= MaxTailRecordBytes && v.buffered_bytes() <= MaxTailRecordBytes,
              "record buffer ceiling");
        at += n;
    }
    measure = false;
    const auto baseline_live = live_allocations;
    for (std::uint64_t seq = 2; seq <= extra + 1; ++seq) {
        small = txn(seq, small_payload);
        check(v.feed(view(small)) == StreamStatus::NeedMore && !v.result(),
              "suffix after initial marker");
        check(live_allocations == baseline_live, "historical decoded effects retained");
    }
    check(v.finish() == StreamStatus::Complete && v.result()->record_count() == extra + 1 &&
              v.result()->observed().sequence.value() == extra + 1 &&
              v.result()->observed().offset.value() == extent &&
              v.result()->observed().record_crc.value() == last_crc &&
              v.result()->observed().prefix_crc.value() == prefix &&
              v.result()->matched_boundary().offset.value() == 96 &&
              largest_allocation == MaxTailRecordBytes && v.buffer_capacity() == MaxTailRecordBytes,
          "large scan or retained-memory proof");
    std::cout << "large tail: " << extent << " bytes; payload " << MaxTailPayload << "; record "
              << large.size() << "; 4093-byte chunks; largest validator allocation "
              << largest_allocation << "; " << extra
              << " later records with constant live allocations\n";
}
} // namespace

int main() {
    try {
        golden_and_partitions();
        positive_semantics();
        metadata_negatives();
        frame_and_sequence_negatives();
        semantic_negatives();
        boundary_negatives();
        resource_tests();
        maximum_record_and_history();
        std::cout << "PASS V2 tail stream: " << positives << " positive scans, " << partitions
                  << " partition scans, " << negatives << " rejections (" << truncations
                  << " truncations), " << resource_cases
                  << " resource/overflow cases; "
                     "frozen E/T/U/C/L, real D/O/R/P, EOF, suffix, V1 semantics; no I/O\n";
    } catch (const std::exception& e) {
        allocation_budget = -1;
        measure = false;
        std::cerr << e.what() << '\n';
        return 1;
    }
}
