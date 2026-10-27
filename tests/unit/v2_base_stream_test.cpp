#include "redisdb/persistence/V2BaseStream.hpp"

#include "redisdb/persistence/AofLog.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
long allocation_budget = -1;
bool measure = false;
std::size_t allocated = 0, largest = 0;
} // namespace
void* operator new(std::size_t n) {
    if (allocation_budget == 0)
        throw std::bad_alloc{};
    if (allocation_budget > 0)
        --allocation_budget;
    if (void* p = std::malloc(n ? n : 1)) {
        if (measure) {
            allocated += n;
            largest = std::max(largest, n);
        }
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace redisdb;
using namespace persistence::v2;
void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
ByteView view(std::string_view bytes) {
    return std::as_bytes(std::span{bytes.data(), bytes.size()});
}
// Independent reference: forward Castagnoli polynomial, reflected input/output.
// Table form keeps the >64 MiB case bounded in runtime in Debug/UBSan too.
std::uint32_t reverse32(std::uint32_t x) {
    std::uint32_t r = 0;
    for (unsigned i = 0; i < 32; ++i) {
        r = (r << 1) | (x & 1U);
        x >>= 1;
    }
    return r;
}
std::uint32_t reference_crc(std::string_view bytes, std::uint32_t previous = 0) {
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
            t[i] = reverse32(i) >> 24;
        return t;
    }();
    auto r = reverse32(~previous);
    for (char c : bytes)
        r = (r << 8) ^ table[(r >> 24) ^ reflected[static_cast<unsigned char>(c)]];
    return ~reverse32(r);
}
std::string be(std::uint64_t n, unsigned width) {
    std::string b;
    for (unsigned i = width; i != 0; --i)
        b.push_back(static_cast<char>((n >> ((i - 1) * 8)) & 255U));
    return b;
}
void put(std::string& b, std::size_t at, std::uint64_t n, unsigned width) {
    b.replace(at, width, be(n, width));
}
std::uint32_t u32(std::string_view b, std::size_t at) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i)
        n = (n << 8) | static_cast<unsigned char>(b[at + i]);
    return n;
}
std::string blob(std::string_view s) { return be(s.size(), 4) + std::string{s}; }
std::string seal(std::string prefix) {
    prefix += be(reference_crc(prefix), 4);
    prefix += "END!";
    return prefix;
}
std::string raw_frame(std::string_view tag, std::string_view payload) {
    return seal(std::string{tag} + be(payload.size(), 4) + std::string{payload});
}
std::string payload(std::string_view key, unsigned tag, std::string value,
                    std::optional<std::int64_t> micros = {}) {
    std::string b = be(1, 4) + blob(key) + be(tag, 1);
    if (tag) {
        b += be(micros.has_value(), 1);
        if (micros)
            b += be(std::bit_cast<std::uint64_t>(*micros), 8);
        b += value;
    }
    b += std::string(5, '\0');
    return b;
}
std::string string_payload(std::string_view key, std::string_view value) {
    return payload(key, 1, blob(value));
}
std::string header(std::uint64_t k, std::uint64_t q, bool lru = false, bool fresh = false) {
    std::string b(176, '\0');
    b.replace(0, 8, "RDBBAS02");
    put(b, 8, 2, 4);
    put(b, 12, lru, 4);
    put(b, 16, 184, 4);
    put(b, 20, 1, 4);
    for (unsigned i = 0; i < 16; ++i)
        b[24 + i] = static_cast<char>(i);
    put(b, 40, fresh ? 1 : 3, 8);
    put(b, 48, fresh ? 0 : 1, 8);
    put(b, 56, k, 8);
    put(b, 64, q, 8);
    put(b, 72, lru ? k : 0, 8);
    if (!fresh) {
        put(b, 80, 2, 4);
        b.replace(88, 16, b.substr(24, 16));
        put(b, 104, 1, 8);
        for (auto at : {112U, 136U}) {
            put(b, at, 1, 8);
            put(b, at + 8, 144, 8);
            put(b, at + 16, 0xad924c74U, 4);
            put(b, at + 20, 0x04430f74U, 4);
        }
    }
    return seal(b);
}
std::string complete(std::string prefix) {
    prefix += raw_frame("DONE", be(reference_crc(prefix), 4));
    return prefix;
}
BaseReferenceFields reference(std::string_view b) {
    auto h = decode_base_header(view(b.substr(0, 184)));
    return {1, h.store, h.generation, h.sequence, b.size(), reference_crc(b), u32(b, 176)};
}
std::size_t rejections = 0, partitions = 0;
void reject(std::string_view bytes, BaseReferenceFields claims,
            std::optional<BaseStreamError> category = {}) {
    BaseStreamValidator v(claims);
    (void)v.feed(view(bytes));
    check(v.finish() == StreamStatus::Error && !v.result(), "corruption accepted");
    if (category && v.error() != *category) {
        std::cerr << "rejection " << rejections << " expected " << static_cast<int>(*category)
                  << " got " << static_cast<int>(v.error()) << '\n';
        check(false, "wrong rejection category");
    }
    const auto error = v.error();
    check(v.feed({}) == StreamStatus::Error && v.feed(view("x")) == StreamStatus::Error &&
              v.finish() == StreamStatus::Error && v.error() == error && !v.result(),
          "failure is not terminal");
    ++rejections;
}
void reject(std::string_view bytes, std::optional<BaseStreamError> category = {}) {
    reject(bytes, reference(bytes), category);
}
void accepted(std::string_view b, std::size_t chunk) {
    auto claims = reference(b);
    BaseStreamValidator v(claims);
    check(v.feed({}) == StreamStatus::NeedMore, "empty feed");
    for (std::size_t at = 0; at < b.size();) {
        const auto n = std::min(chunk, b.size() - at);
        check(v.feed(view(b.substr(at, n))) == StreamStatus::NeedMore && !v.result(),
              "accepted before EOF");
        at += n;
    }
    check(v.finish() == StreamStatus::Complete && v.result(), "valid base rejected");
    const auto& r = *v.result();
    check(r.identity().file_crc.value() == reference_crc(b) &&
              r.identity().byte_length == b.size() && r.entries().size() == r.header().key_count &&
              r.lru_order().size() == r.header().lru_count &&
              r.prefix_crc().value() == reference_crc(b.substr(0, b.size() - 20)) &&
              r.done_crc().value() == u32(b, b.size() - 8),
          "verified metadata differs");
    check(v.finish() == StreamStatus::Complete, "finish not idempotent");
    check(v.feed({}) == StreamStatus::Error && v.error() == BaseStreamError::Finalized &&
              !v.result(),
          "feed after success");
    ++partitions;
}

void golden_and_partitions() {
    check(reference_crc("123456789") == 0xe3069283U, "independent CRC");
    const auto e = complete(header(0, 0, false, true));
    const std::string key("\0k", 2), value("\xff\0v", 3);
    const auto key_frame = raw_frame("KEY1", string_payload(key, value));
    const auto c = complete(header(1, 69) + key_frame);
    auto lh = header(2, 135, true);
    put(lh, 40, 4, 8);
    put(lh, 48, 2, 8);
    for (auto at : {112U, 136U}) {
        put(lh, at, 2, 8);
        put(lh, at + 8, 200, 8);
        put(lh, at + 16, 0xe91dbec0U, 4);
        put(lh, at + 20, 0xa33c606bU, 4);
    }
    lh = seal(lh.substr(0, 176));
    const std::string high(1, '\xff');
    const auto l = complete(lh + key_frame + raw_frame("KEY1", string_payload(high, "7")) +
                            raw_frame("LRU1", high) + raw_frame("LRU1", key));
    // Frozen section-15 full-file checksums, lengths and stored header checksums.
    check(e.size() == 204 && reference_crc(e) == 0x4ebad660U && u32(e, 176) == 0xbc252022U &&
              c.size() == 244 && reference_crc(c) == 0x155d9af1U && u32(c, 176) == 0x213707daU &&
              l.size() == 316 && reference_crc(l) == 0x6ddee862U && u32(l, 176) == 0xa646354bU,
          "frozen E/C/L vectors");
    for (const auto& b : {e, c, l}) {
        accepted(b, b.size());
        accepted(b, 1);
        accepted(b, 7);
        // Every possible two-chunk split covers every fixed field, CRC/trailer.
        for (std::size_t split = 0; split <= b.size(); ++split) {
            BaseStreamValidator v(reference(b));
            check(v.feed(view(std::string_view{b}.substr(0, split))) == StreamStatus::NeedMore &&
                      v.feed(view(std::string_view{b}.substr(split))) == StreamStatus::NeedMore &&
                      !v.result() && v.finish() == StreamStatus::Complete,
                  "two-chunk split");
            ++partitions;
        }
        BaseStreamValidator irregular(reference(b));
        std::size_t at = 0, step = 1;
        while (at < b.size()) {
            auto n = std::min(step, b.size() - at);
            std::string owned = b.substr(at, n);
            check(irregular.feed(view(owned)) == StreamStatus::NeedMore, "irregular partition");
            std::fill(owned.begin(), owned.end(), '?'); // destroy caller bytes immediately
            at += n;
            step = (step * 17) % 43 + 1;
        }
        check(irregular.finish() == StreamStatus::Complete, "borrowed chunk survived feed");
        ++partitions;
        // Every truncation, including all payload/checksum/trailer boundaries.
        for (std::size_t n = 0; n < b.size(); ++n)
            reject(std::string_view{b}.substr(0, n), reference(b));
    }
    const auto decoded = persistence::decode_effects(string_payload(key, value));
    check(encode_key_frame(decoded.entries.front()) == key_frame &&
              encode_lru_frame(key) == raw_frame("LRU1", key) &&
              encode_done_frame(Crc32c{reference_crc(e.substr(0, 184))}) == e.substr(184),
          "encoder differs from independent V1 frame representation");
    BaseStreamValidator identity(BaseIdentity{StoreId{reference(c).store},
                                              GenerationId{std::uint64_t{3}},
                                              LogicalSequence{std::uint64_t{1}}, c.size(),
                                              Crc32c{u32(c, 176)}, Crc32c{reference_crc(c)}});
    check(identity.feed(view(c)) == StreamStatus::NeedMore &&
              identity.finish() == StreamStatus::Complete,
          "BaseIdentity overload");
}

std::string typed_base(bool lru) {
    const std::string nul(1, '\0'), binary("\0\xff", 2), hi(1, '\x80'), last(1, '\xff');
    auto b = header(8, 1096, lru);
    for (const auto& p :
         {string_payload("", ""), string_payload(nul, binary),
          payload("h", 3, be(2, 4) + blob("z") + blob("v") + blob("a") + blob("")),
          payload("l", 2, be(3, 4) + blob("") + blob("x") + blob("x")),
          payload("n", 1, blob("0007"), -1), payload("s", 4, be(2, 4) + blob("z") + blob("a")),
          payload(hi, 1, blob(""), 1), payload(last, 1, blob("9"), 123456789)})
        b += raw_frame("KEY1", p);
    if (lru)
        for (const auto& k : {last, hi, std::string{"s"}, std::string{"n"}, std::string{"l"},
                              std::string{"h"}, nul, std::string{}})
            b += raw_frame("LRU1", k);
    return complete(b);
}
void types_and_ownership() {
    for (bool history : {false, true}) {
        auto b = typed_base(history);
        accepted(b, 1);
        BaseStreamValidator v(reference(b));
        check(v.feed(view(b)) == StreamStatus::NeedMore, "typed feed");
        std::fill(b.begin(), b.end(), '?');
        check(v.finish() == StreamStatus::Complete, "owned state");
        const auto& r = *v.result();
        check(r.header().logical_charge == 1096 && r.entries().size() == 8, "physical ledger");
        const auto entries = r.entries();
        check(entries[0].key.empty() &&
                  std::get<storage::StringValue>(entries[0].replacement->value).bytes.empty() &&
                  entries[1].key == std::string(1, '\0') &&
                  std::get<storage::StringValue>(entries[1].replacement->value).bytes ==
                      std::string("\0\xff", 2),
              "binary/empty state");
        check(std::get<storage::HashValue>(entries[2].replacement->value).fields ==
                      storage::HashValue{{{"z", "v"}, {"a", ""}}}.fields &&
                  std::get<storage::ListValue>(entries[3].replacement->value).elements ==
                      std::list<std::string>({"", "x", "x"}) &&
                  std::get<storage::SetValue>(entries[5].replacement->value).members ==
                      storage::SetValue{{"z", "a"}}.members,
              "unsorted inner uniqueness and ordered list duplicates");
        check(std::get<storage::StringValue>(entries[4].replacement->value).bytes == "0007",
              "integer-looking bytes normalized");
        std::size_t q = 0;
        for (const auto& e : entries)
            q += e.replacement->accounted_charge;
        check(q == 1096 &&
                  entries[4].replacement->expires_at ==
                      storage::WallTimePoint{std::chrono::microseconds{-1}} &&
                  entries[6].replacement->expires_at ==
                      storage::WallTimePoint{std::chrono::microseconds{1}} &&
                  entries[7].replacement->expires_at ==
                      storage::WallTimePoint{std::chrono::microseconds{123456789}},
              "absolute negative/positive/past TTL or expired physical charge");
        if (history) {
            check(r.lru_order().size() == 8 && r.lru_order()[0] == 7 && r.lru_order()[7] == 0,
                  "oldest-to-newest permutation");
        } else {
            check(r.lru_order().empty() && entries[6].key == std::string(1, '\x80') &&
                      entries[7].key == std::string(1, '\xff'),
                  "cold binary order");
        }
        for (const auto& e : entries) {
            const auto encoded = encode_key_frame(e);
            storage::CommittedEffects one;
            one.entries.push_back(e);
            check(encoded == raw_frame("KEY1", persistence::encode_effects(one)),
                  "typed encoder V1 bytes");
        }
    }
    accepted(complete(header(0, 0, true)), 1); // empty LRU, compaction provenance
    using Duration = storage::WallTimePoint::duration;
    constexpr auto scale = Duration::period::den / 1'000'000;
    for (const auto deadline : {INT64_MIN / scale, INT64_MAX / scale})
        accepted(complete(header(1, 114) + raw_frame("KEY1", payload("a", 1, blob(""), deadline))),
                 1);
}

void malformed() {
    const auto good = complete(header(1, 66) + raw_frame("KEY1", string_payload("a", "v")));
    const auto claims = reference(good);
    const auto empty = complete(header(0, 0, false, true));
    const auto ec = reference(empty);
    for (auto offset : {0U, 8U, 12U, 176U, 180U}) {
        auto b = good;
        b[offset] ^= 1;
        reject(b, claims);
    }
    for (auto edit : {+[](BaseReferenceFields& r) { r.store[0] ^= std::byte{1}; },
                      +[](BaseReferenceFields& r) { ++r.generation; },
                      +[](BaseReferenceFields& r) { ++r.sequence; },
                      +[](BaseReferenceFields& r) { r.header_crc ^= 1; },
                      +[](BaseReferenceFields& r) { r.file_crc ^= 1; },
                      +[](BaseReferenceFields& r) { ++r.byte_length; },
                      +[](BaseReferenceFields& r) { --r.byte_length; },
                      +[](BaseReferenceFields& r) { r.byte_length = UINT64_MAX; },
                      +[](BaseReferenceFields& r) { r.accounting_model = 2; },
                      +[](BaseReferenceFields& r) { r.store = {}; }}) {
        auto r = claims;
        edit(r);
        reject(good, r);
    }
    reject(good.substr(0, good.size() - 20), claims); // missing DONE
    for (const auto& suffix : {std::string{"x"}, empty.substr(184),
                               raw_frame("KEY1", string_payload("z", "")), raw_frame("LRU1", "")}) {
        auto b = good + suffix;
        auto r = claims;
        r.byte_length = b.size();
        r.file_crc = reference_crc(b);
        reject(b, r, BaseStreamError::TrailingBytes);
    }
    for (const auto tag : {"XXXX", "LRU1", "DONE"})
        reject(complete(header(1, 66) + raw_frame(tag, string_payload("a", "v"))),
               BaseStreamError::Frame);
    // DONE is present, but the KEY/LRU grammar has already finished.
    reject(complete(header(0, 0) + raw_frame("KEY1", string_payload("", ""))),
           BaseStreamError::Frame);
    reject(complete(header(0, 0) + raw_frame("LRU1", "")), BaseStreamError::Frame);
    for (auto length : {0U, 3U, 5U, UINT32_MAX}) {
        auto b = empty;
        put(b, 188, length, 4);
        reject(b, ec, BaseStreamError::Length);
    }
    for (auto length : {static_cast<std::uint32_t>(MaxKeyPayload + 1), UINT32_MAX}) {
        auto b = good;
        put(b, 188, length, 4);
        reject(b, claims, BaseStreamError::Length);
    }
    auto bad = good;
    put(bad, 188, 1000, 4); // capped but beyond remaining extent
    reject(bad, claims, BaseStreamError::Length);
    bad = good;
    bad[good.size() - 28] ^= 1; // KEY1 checksum
    reject(bad, claims, BaseStreamError::Checksum);
    bad = good;
    bad[good.size() - 21] ^= 1; // KEY1 trailer
    reject(bad, claims, BaseStreamError::Trailer);
    bad = good.substr(0, good.size() - 20) + raw_frame("DONE", be(0, 4));
    reject(bad, BaseStreamError::Checksum);
    bad = good;
    bad.back() = '?';
    reject(bad, claims, BaseStreamError::Trailer);
    reject(complete(header(1, 67) + raw_frame("KEY1", string_payload("a", "v"))),
           BaseStreamError::Charge);
    reject(complete(header(2, 128) + raw_frame("KEY1", string_payload("a", "v"))));
    reject(complete(header(1'048'577, 64ULL * 1'048'577)), BaseStreamError::Length);
    bad = good;
    put(bad, 56, UINT64_MAX, 8);
    bad.replace(176, 8, seal(bad.substr(0, 176)).substr(176));
    reject(bad, claims, BaseStreamError::Header);

    // Each malformed semantic payload gets independent enclosing frame/prefix/file CRCs.
    std::vector<std::string> invalid{
        payload("tombstone", 0, ""), // enough bytes to pass the minimum-size check
        payload("a", 5, blob("v")),  // inherited type tag
        payload("a", 2, be(0, 4)),
        payload("a", 3, be(0, 4)),
        payload("a", 4, be(0, 4)),
        payload("a", 3, be(2, 4) + blob("z") + blob("1") + blob("z") + blob("2")),
        payload("a", 4, be(2, 4) + blob("z") + blob("z")),
        payload("a", 3, be(1'048'577, 4)),         // inherited item bound
        payload("a", 3, be(100, 4) + blob("")),    // count lacks sufficient bytes
        payload("a", 1, be(MaxLruPayload + 1, 4)), // oversized blob
        payload("a", 1, blob("v")) + "suffix"};
    auto p = string_payload("a", "v");
    p[10] = 2; // invalid timed flag: count4 + blob5 + tag1
    invalid.push_back(p);
    p = string_payload("a", "v");
    p[p.size() - 5] = 1; // even has_lru true with empty order rejects KEY1
    invalid.push_back(p);
    p = string_payload("a", "v");
    p.replace(p.size() - 5, 5, be(1, 1) + be(1, 4) + blob("a"));
    invalid.push_back(p);
    p = string_payload("a", "v");
    put(p, 0, 2, 4);
    p.insert(p.size() - 5, string_payload("b", "v").substr(4, 12));
    invalid.push_back(p);
    p = string_payload("a", "v");
    put(p, 4, UINT32_MAX, 4); // key representation
    invalid.push_back(p);
    using Duration = storage::WallTimePoint::duration;
    constexpr auto scale = Duration::period::den / 1'000'000;
    if constexpr (scale > 1) {
        invalid.push_back(payload("a", 1, blob("v"), INT64_MAX));
        invalid.push_back(payload("a", 1, blob("v"), INT64_MIN));
    }
    for (const auto& value : invalid)
        reject(complete(header(1, 64) + raw_frame("KEY1", value)), BaseStreamError::Semantic);
    for (const auto& keys : {std::vector<std::string>{"a", "a"},
                             {"b", "a"},
                             {"ab", "a"},
                             {std::string(1, '\xff'), std::string(1, '\x80')}})
        reject(complete(header(2, 128) + raw_frame("KEY1", string_payload(keys[0], "")) +
                        raw_frame("KEY1", string_payload(keys[1], ""))),
               BaseStreamError::Order);
    const auto prefix = header(2, 170, true) +
                        raw_frame("KEY1", string_payload("a", std::string(20, 'x'))) +
                        raw_frame("KEY1", string_payload("b", std::string(20, 'x')));
    reject(complete(prefix + raw_frame("LRU1", "a") + raw_frame("LRU1", "a")),
           BaseStreamError::Permutation);
    reject(complete(prefix + raw_frame("LRU1", "z") + raw_frame("LRU1", "b")),
           BaseStreamError::Permutation);
    reject(complete(prefix + raw_frame("LRU1", "a")), BaseStreamError::Frame);
    bad = complete(prefix + raw_frame("LRU1", "a") + raw_frame("LRU1", "b"));
    put(bad, prefix.size() + 4, MaxLruPayload + 1, 4);
    reject(bad, reference(bad), BaseStreamError::Length);
    bad = good;
    put(bad, 72, 1, 8); // LRU without flag, valid own header CRC
    bad.replace(176, 8, seal(bad.substr(0, 176)).substr(176));
    reject(bad, claims, BaseStreamError::Header);
}

void resources() {
    // Aggregate K is u64, independently of the inherited per-payload 1M limit.
    // A feasible large claim alone must not allocate an entry array or reject K.
    const auto aggregate_header = header(1'048'577, 64ULL * 1'048'577);
    auto aggregate_ref = reference(complete(aggregate_header));
    aggregate_ref.byte_length = 204ULL + 35ULL * 1'048'577;
    BaseStreamValidator aggregate(aggregate_ref);
    allocated = 0;
    measure = true;
    const auto aggregate_status = aggregate.feed(view(aggregate_header));
    measure = false;
    check(aggregate_status == StreamStatus::NeedMore && allocated < 4096 &&
              aggregate.finish() == StreamStatus::Error,
          "aggregate count caused reserve or inherited 1M cap");
    const auto b = typed_base(true);
    for (auto limits :
         {BaseStreamLimits{1, SIZE_MAX, SIZE_MAX}, BaseStreamLimits{MaxKeyPayload, 7, SIZE_MAX},
          BaseStreamLimits{MaxKeyPayload, SIZE_MAX, 1095}}) {
        BaseStreamValidator v(reference(b), limits);
        check(v.feed(view(b)) == StreamStatus::Error && !v.result() &&
                  v.error() == BaseStreamError::ResourceLimit,
              "caller budget");
    }
    bool success = false;
    std::size_t failures = 0;
    for (long at = 0; at < 500; ++at) {
        BaseStreamValidator v(reference(b));
        allocation_budget = at;
        const auto status = v.feed(view(b));
        const auto final = v.finish();
        allocation_budget = -1;
        if (status == StreamStatus::Error) {
            check(final == StreamStatus::Error && !v.result() &&
                      v.error() == BaseStreamError::ResourceLimit,
                  "allocation failure escaped");
            ++failures;
        } else {
            check(final == StreamStatus::Complete, "allocation campaign success");
            success = true;
            break;
        }
    }
    check(success && failures > 20, "allocation campaign coverage");
    // Allocation work remains linear/amortized in bytes, independent of chunk count.
    const auto modest = complete(header(1, 64 + 1 + 8192) +
                                 raw_frame("KEY1", string_payload("a", std::string(8192, 'x'))));
    std::array<std::size_t, 2> totals{};
    for (std::size_t trial = 0; trial < 2; ++trial) {
        BaseStreamValidator v(reference(modest));
        allocated = 0;
        measure = true;
        const auto chunk = trial == 0 ? modest.size() : 1;
        for (std::size_t at = 0; at < modest.size();) {
            const auto n = std::min(chunk, modest.size() - at);
            (void)v.feed(view(std::string_view{modest}.substr(at, n)));
            at += n;
        }
        (void)v.finish();
        measure = false;
        check(v.status() == StreamStatus::Complete, "allocation measurement validation");
        totals[trial] = allocated;
    }
    check(totals[1] <= totals[0] + 1024, "quadratic chunk allocation");
    auto encoding_fails = [](auto fn) {
        bool failed = false;
        try {
            fn();
        } catch (const BaseFrameError&) {
            failed = true;
        } catch (const persistence::PersistenceError&) {
            failed = true;
        }
        check(failed, "invalid frame encoder input accepted");
    };
    encoding_fails([] { (void)encode_key_frame({"a", std::nullopt}); });
    encoding_fails([] { (void)encode_key_frame({"a", storage::Entry{storage::ListValue{}}}); });
    encoding_fails([] { (void)encode_key_frame({"a", storage::Entry{storage::HashValue{}}}); });
    encoding_fails([] { (void)encode_key_frame({"a", storage::Entry{storage::SetValue{}}}); });
    encoding_fails([] { (void)encode_lru_frame(std::string(MaxLruPayload + 1, 'x')); });
    encoding_fails([] {
        (void)encode_key_frame(
            {"a", storage::Entry{storage::StringValue{std::string(MaxLruPayload + 1, 'x')}}});
    });
    std::cout << "allocation failures reached: " << failures << '\n';
}

void large_base() {
    // Two passes over individual frames; never construct the complete 72 MiB base.
    // Nine 8 MiB values, each valid under its blob and per-frame bounds.
    constexpr std::size_t value_size = MaxLruPayload;
    const auto h = header(9, 9ULL * (65 + value_size));
    auto crc = reference_crc(h);
    std::uint64_t total = h.size();
    for (unsigned i = 0; i < 9; ++i) {
        const auto f =
            raw_frame("KEY1", string_payload(be('0' + i, 1), std::string(value_size, 'x')));
        crc = reference_crc(f, crc);
        total += f.size();
    }
    const auto done = raw_frame("DONE", be(crc, 4));
    total += done.size();
    const auto file_crc = reference_crc(done, crc);
    auto r = reference(complete(h)); // only identity/header fields are reused
    r.byte_length = total;
    r.file_crc = file_crc;
    BaseStreamValidator v(r);
    check(v.feed(view(h)) == StreamStatus::NeedMore, "large header");
    largest = 0;
    for (unsigned i = 0; i < 9; ++i) {
        const auto f =
            raw_frame("KEY1", string_payload(be('0' + i, 1), std::string(value_size, 'x')));
        measure = true;
        for (std::size_t at = 0; at < f.size();) {
            const auto n = std::min<std::size_t>(4093, f.size() - at);
            check(v.feed(view(std::string_view{f}.substr(at, n))) == StreamStatus::NeedMore,
                  "large fragmented frame");
            check(v.buffered_bytes() <= value_size + 36, "whole-base buffering");
            at += n;
        }
        measure = false;
    }
    check(v.feed(view(done)) == StreamStatus::NeedMore && !v.result() &&
              v.finish() == StreamStatus::Complete && v.result()->entries().size() == 9 &&
              total > MaxKeyPayload && largest < 2 * value_size,
          "large base capped or whole-base allocation");
    std::cout << "large base: " << total
              << " bytes, 4093-byte chunks, largest validator allocation " << largest << '\n';
}

void maximum_frame() {
    // KEY1 minimum for an empty-key list of eight elements is 51 bytes before
    // element data. This creates an exactly 64 MiB payload, independently.
    std::string value = be(8, 4);
    value.reserve(MaxKeyPayload);
    for (unsigned i = 0; i < 8; ++i)
        value += blob(std::string(MaxLruPayload - (i == 7 ? 51 : 0), 'x'));
    auto p = payload("", 2, std::move(value));
    check(p.size() == MaxKeyPayload, "max payload fixture arithmetic");
    auto f = raw_frame("KEY1", p);
    p.clear();
    p.shrink_to_fit();
    const auto h = header(1, MaxKeyPayload + 397ULL);
    const auto prefix = reference_crc(f, reference_crc(h));
    const auto done = raw_frame("DONE", be(prefix, 4));
    auto r = reference(complete(h));
    r.byte_length = h.size() + f.size() + done.size();
    r.file_crc = reference_crc(done, prefix);
    BaseStreamValidator v(r);
    check(v.feed(view(h)) == StreamStatus::NeedMore, "max-frame header");
    for (std::size_t at = 0; at < f.size();) {
        const auto n = std::min<std::size_t>(8191, f.size() - at);
        check(v.feed(view(std::string_view{f}.substr(at, n))) == StreamStatus::NeedMore,
              "max frame partition");
        at += n;
    }
    check(v.feed(view(done)) == StreamStatus::NeedMore && v.finish() == StreamStatus::Complete,
          "exact 64 MiB payload rejected");
}
} // namespace

int main() {
    try {
        golden_and_partitions();
        types_and_ownership();
        malformed();
        resources();
        large_base();
        maximum_frame();
        std::cout << "PASS V2 base stream: " << partitions << " partitions, " << rejections
                  << " rejections; E/C/L, types/TTL/physical charge/permutation, explicit EOF, "
                     "resource failures, >64 MiB base and exact-limit frame; no I/O\n";
    } catch (const std::exception& e) {
        allocation_budget = -1;
        measure = false;
        std::cerr << e.what() << '\n';
        return 1;
    } catch (BaseStreamError) {
        std::cerr << "unexpected encoder rejection\n";
        return 1;
    }
}
