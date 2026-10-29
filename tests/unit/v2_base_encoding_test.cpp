#include "redisdb/persistence/V2BaseEncoding.hpp"

#include "redisdb/persistence/AofLog.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <type_traits>
#include <utility>

namespace {
struct alignas(std::max_align_t) Allocation {
    std::size_t size;
};
struct AllocationPoint {
    std::size_t size, operation;
};
std::array<AllocationPoint, 8192> allocation_trace{};
std::size_t live_allocations = 0, live_bytes = 0;
std::size_t allocation_calls = 0, armed = 0, reached = 0, operation = 0;
std::size_t largest_request = 0, peak_live = 0;
bool observe_allocations = false;
} // namespace
void* operator new(std::size_t n) {
    if (observe_allocations) {
        if (allocation_calls == allocation_trace.size())
            std::abort();
        allocation_trace[allocation_calls++] = {n, operation};
        largest_request = std::max(largest_request, n);
        if (armed && allocation_calls == armed) {
            ++reached;
            throw std::bad_alloc{};
        }
    }
    if (n > SIZE_MAX - sizeof(Allocation))
        throw std::bad_alloc{};
    auto* block = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + (n ? n : 1)));
    if (!block)
        throw std::bad_alloc{};
    block->size = n;
    ++live_allocations;
    live_bytes += n;
    if (observe_allocations)
        peak_live = std::max(peak_live, live_bytes);
    return block + 1;
}
[[gnu::noinline]] void operator delete(void* p) noexcept {
    if (p) {
        auto* block = static_cast<Allocation*>(p) - 1;
        live_bytes -= block->size;
        --live_allocations;
        std::free(block);
    }
}
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
[[gnu::noinline]] void operator delete[](void* p) noexcept { ::operator delete(p); }
[[gnu::noinline]] void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace redisdb;
using namespace persistence::v2;
static_assert(std::is_final_v<BaseEncodingCursor>);
static_assert(!std::is_copy_constructible_v<BaseEncodingCursor>);
static_assert(!std::is_copy_assignable_v<BaseEncodingCursor>);
static_assert(std::is_nothrow_move_constructible_v<BaseEncodingCursor>);
static_assert(std::is_nothrow_move_assignable_v<BaseEncodingCursor>);
static_assert(std::is_nothrow_destructible_v<BaseEncodingCursor>);
static_assert(!std::is_default_constructible_v<EncodedBaseSummary>);
static_assert(!std::is_convertible_v<EncodedBaseSummary, ValidatedBase>);
static_assert(std::is_same_v<decltype(std::declval<BaseEncodingCursor&>().bytes()), ByteView>);
static_assert(noexcept(BaseEncodingCursor::begin(std::declval<BaseEncodingContext>(),
                                                 std::declval<BaseInputView>(),
                                                 std::declval<BaseEncodeLimits>())));
static_assert(noexcept(std::declval<BaseEncodingCursor&>().next()));
template <class T>
concept ForbiddenApi =
    requires(T& t) { t.write(ByteView{}); } || requires(T& t) { t.append(ByteView{}); } ||
    requires(T& t) { t.seal_file(); } || requires(T& t) { t.publish(); } ||
    requires(T& t) { t.install(); } || requires(T& t) { t.activate(); } ||
    requires(T& t) { t.fd(); } || requires(T& t) { t.filename(); } ||
    requires(T& t) { t.staged(); } || requires(T& t) { t.root(); };
static_assert(!ForbiddenApi<BaseEncodingCursor>);
std::size_t scenarios = 0, injections = 0;
void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
template <class Fn> void scenario(const char* name, Fn fn) {
    fn();
    ++scenarios;
    std::cout << "SCENARIO " << scenarios << " PASS " << name << '\n';
}
ByteView bytes(std::string_view value) {
    return std::as_bytes(std::span{value.data(), value.size()});
}
std::string text(ByteView value) {
    if (value.empty())
        return {};
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
bool same_bytes(ByteView a, std::string_view b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
std::uint32_t reverse(std::uint32_t n) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 32; ++i) {
        result = (result << 1) | (n & 1U);
        n >>= 1;
    }
    return result;
}
// Independent forward Castagnoli polynomial, reflected input/output.
std::uint32_t crc(std::string_view data, std::uint32_t prior = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto r = i << 24;
            for (unsigned j = 0; j < 8; ++j)
                r = (r << 1) ^ ((r & 0x80000000U) ? 0x1edc6f41U : 0U);
            result[i] = r;
        }
        return result;
    }();
    static const auto reflected = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i)
            result[i] = reverse(i) >> 24;
        return result;
    }();
    auto r = reverse(~prior);
    for (const char c : data)
        r = (r << 8) ^ table[(r >> 24) ^ reflected[static_cast<unsigned char>(c)]];
    return ~reverse(r);
}
void put(std::string& target, std::size_t at, std::uint64_t n, unsigned width) {
    check(at <= target.size() && width <= target.size() - at, "oracle BE bounds");
    for (unsigned i = width; i != 0; --i)
        target[at + width - i] = static_cast<char>((n >> ((i - 1) * 8)) & 255U);
}
std::string be(std::uint64_t n, unsigned width) {
    std::string result(width, '\0');
    put(result, 0, n, width);
    return result;
}
std::uint32_t get32(std::string_view data, std::size_t at) {
    check(at <= data.size() && 4 <= data.size() - at, "oracle u32 bounds");
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i)
        n = (n << 8) | static_cast<unsigned char>(data[at + i]);
    return n;
}
std::string seal(std::string prefix) { return prefix + be(crc(prefix), 4) + "END!"; }
std::string frame(std::string_view tag, std::string_view payload) {
    return seal(std::string{tag} + be(payload.size(), 4) + std::string{payload});
}
std::string blob(std::string_view s) { return be(s.size(), 4) + std::string{s}; }
std::int64_t micros(const storage::Entry& e) {
    return std::chrono::floor<std::chrono::microseconds>(e.expires_at->time_since_epoch()).count();
}
std::string value_bytes(const storage::Entry& e) {
    std::string result;
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, storage::StringValue>) {
                result = blob(value.bytes);
            } else if constexpr (std::is_same_v<T, storage::ListValue>) {
                result = be(value.elements.size(), 4);
                for (const auto& item : value.elements)
                    result += blob(item);
            } else if constexpr (std::is_same_v<T, storage::HashValue>) {
                std::vector<std::string> keys;
                for (const auto& [key, field_value] : value.fields) {
                    (void)field_value;
                    keys.push_back(key);
                }
                std::sort(keys.begin(), keys.end());
                result = be(keys.size(), 4);
                for (const auto& key : keys)
                    result += blob(key) + blob(value.fields.at(key));
            } else {
                std::vector<std::string> members(value.members.begin(), value.members.end());
                std::sort(members.begin(), members.end());
                result = be(members.size(), 4);
                for (const auto& member : members)
                    result += blob(member);
            }
        },
        e.value);
    return result;
}
std::string key_frame(std::string_view key, const storage::Entry& e) {
    auto payload =
        be(1, 4) + blob(key) + be(e.value.index() + 1, 1) + be(e.expires_at.has_value(), 1);
    if (e.expires_at)
        payload += be(std::bit_cast<std::uint64_t>(micros(e)), 8);
    payload += value_bytes(e);
    payload += std::string(5, '\0');
    return frame("KEY1", payload);
}
std::size_t charge(std::string_view key, const storage::Entry& e) {
    std::size_t result = 64 + key.size() + (e.expires_at ? 48 + key.size() : 0);
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, storage::StringValue>) {
                result += value.bytes.size();
            } else if constexpr (std::is_same_v<T, storage::ListValue>) {
                for (const auto& item : value.elements)
                    result += 48 + item.size();
            } else if constexpr (std::is_same_v<T, storage::HashValue>) {
                for (const auto& [field, item] : value.fields)
                    result += 80 + field.size() + item.size();
            } else {
                for (const auto& member : value.members)
                    result += 56 + member.size();
            }
        },
        e.value);
    return result;
}
BaseEncodingContext context() {
    StoreBytes store{};
    for (std::size_t i = 0; i < store.size(); ++i)
        store[i] = static_cast<std::byte>(i);
    return {StoreId{store},
            GenerationId{std::uint64_t{1}},
            GenerationId{std::uint64_t{3}},
            LogicalSequence{std::uint64_t{1}},
            {1, 144, 0xad924c74U, 0x04430f74U}};
}
BaseEncodeLimits limits() { return {SIZE_MAX, MaxFileBytes, MaxKeyPayload, SIZE_MAX, SIZE_MAX}; }
struct Fixture {
    BaseEncodingContext ctx = context();
    std::vector<std::string> keys;
    std::vector<storage::Entry> entries;
    std::vector<BaseEntryView> views;
    std::vector<std::size_t> order;
    void add(std::string key, storage::Value value,
             std::optional<storage::WallTimePoint> deadline = {}) {
        keys.push_back(std::move(key));
        entries.push_back(storage::Entry{std::move(value), deadline});
        entries.back().accounted_charge = 999; // Deliberately ignored cached charge.
    }
    void bind() {
        views.clear();
        order.clear();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            views.push_back({keys[i], &entries[i]});
            order.push_back(entries.size() - 1 - i);
        }
    }
    BaseInputView input(bool lru) const {
        return {views, lru,
                lru ? std::span<const std::size_t>{order} : std::span<const std::size_t>{}};
    }
    std::string fingerprint() const {
        std::string result;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            result += blob(keys[i]) + value_bytes(entries[i]);
            result += be(entries[i].accounted_charge, 8);
            result += be(entries[i].expires_at.has_value(), 1);
            if (entries[i].expires_at)
                result += be(
                    std::bit_cast<std::uint64_t>(entries[i].expires_at->time_since_epoch().count()),
                    8);
        }
        for (const auto i : order)
            result += be(i, 8);
        return result;
    }
};
std::string header(const Fixture& f, bool lru) {
    std::string b(176, '\0');
    b.replace(0, 8, "RDBBAS02");
    put(b, 8, 2, 4);
    put(b, 12, lru, 4);
    put(b, 16, 184, 4);
    put(b, 20, 1, 4);
    for (std::size_t i = 0; i < 16; ++i) {
        b[24 + i] = std::bit_cast<char>(std::to_integer<unsigned char>(f.ctx.store.bytes()[i]));
        b[88 + i] = b[24 + i];
    }
    put(b, 40, f.ctx.candidate_generation.value(), 8);
    put(b, 48, f.ctx.captured_sequence.value(), 8);
    put(b, 56, f.views.size(), 8);
    std::size_t q = 0;
    for (const auto& view : f.views)
        q += charge(view.key, *view.entry);
    put(b, 64, q, 8);
    put(b, 72, lru ? f.views.size() : 0, 8);
    put(b, 80, 2, 4);
    put(b, 104, f.ctx.selected_generation.value(), 8);
    for (const auto at : {112U, 136U}) {
        put(b, at, f.ctx.synchronized_source.sequence, 8);
        put(b, at + 8, f.ctx.synchronized_source.offset, 8);
        put(b, at + 16, f.ctx.synchronized_source.record_crc, 4);
        put(b, at + 20, f.ctx.synchronized_source.prefix_crc, 4);
    }
    return seal(b);
}
std::string reference(const Fixture& f, bool lru) {
    auto b = header(f, lru);
    for (const auto& view : f.views)
        b += key_frame(view.key, *view.entry);
    if (lru)
        for (const auto i : f.order)
            b += frame("LRU1", f.views[i].key);
    return b + frame("DONE", be(crc(b), 4));
}
BaseReferenceFields claims(std::string_view data, const BaseEncodingContext& ctx) {
    return {1,
            ctx.store.bytes(),
            ctx.candidate_generation.value(),
            ctx.captured_sequence.value(),
            data.size(),
            crc(data),
            data.size() >= 184 ? get32(data, 176) : 0};
}
std::string collect(BaseEncodingCursor& cursor) {
    std::string result;
    for (;;) {
        const auto step = cursor.next();
        check(step != BaseEncodeStep::Error, "unexpected cursor error");
        if (step == BaseEncodeStep::End)
            return result;
        check(!cursor.bytes().empty() && !cursor.complete() && !cursor.result(),
              "live chunk/early result");
        result += text(cursor.bytes());
    }
}
void terminal(BaseEncodingCursor& c, BaseEncodeError expected) {
    const auto first = c.error();
    check(first.code == expected && !c.complete() && !c.result() && c.bytes().empty(),
          "wrong terminal rejection");
    allocation_calls = armed = 0;
    observe_allocations = true;
    bool good = true;
    for (unsigned i = 0; i < 3; ++i) {
        const auto next = c.next();
        const auto e = c.error();
        good = good && next == BaseEncodeStep::Error && e.code == first.code &&
               e.fixed_format == first.fixed_format && e.frame_format == first.frame_format &&
               c.bytes().empty() && !c.result() && !c.complete();
    }
    observe_allocations = false;
    check(good && allocation_calls == 0, "terminal failure recovered/allocated");
}
void accept(Fixture& f, bool lru) {
    const auto before = f.fingerprint(), expected = reference(f, lru);
    auto c = BaseEncodingCursor::begin(f.ctx, f.input(lru), limits());
    check(c.error().code == BaseEncodeError::None && c.bytes().empty() && !c.result(),
          "begin valid input");
    const auto actual = collect(c);
    check(actual == expected && c.complete() && c.result() && c.bytes().empty(),
          "independent complete bytes differ");
    const auto& r = *c.result();
    check(r.identity().byte_length == expected.size() && c.bytes_emitted() == expected.size() &&
              r.identity().header_crc.value() == get32(expected, 176) &&
              r.identity().file_crc.value() == crc(expected) &&
              r.prefix_crc().value() ==
                  crc(std::string_view{expected}.substr(0, expected.size() - 20)) &&
              r.done_crc().value() == get32(expected, expected.size() - 8) &&
              r.header() == decode_base_header(bytes(std::string_view{expected}.substr(0, 184))),
          "identity/checksum/extent summary");
    for (const auto chunk : {std::size_t{1}, std::size_t{7}, expected.size()}) {
        BaseStreamValidator v(r.identity());
        for (std::size_t at = 0; at < actual.size(); at += std::min(chunk, actual.size() - at)) {
            const auto n = std::min(chunk, actual.size() - at);
            check(v.feed(bytes(std::string_view{actual}.substr(at, n))) == StreamStatus::NeedMore,
                  "independent decoder feed");
        }
        check(!v.result() && v.finish() == StreamStatus::Complete && v.result(),
              "explicit EOF semantic validation");
        check(v.result()->entries().size() == f.views.size(), "decoded physical count");
        for (std::size_t i = 0; i < f.views.size(); ++i) {
            const auto& decoded = v.result()->entries()[i];
            const auto& original = *f.views[i].entry;
            check(decoded.key == f.views[i].key && decoded.replacement &&
                      decoded.replacement->value == original.value &&
                      decoded.replacement->accounted_charge == charge(decoded.key, original),
                  "decoded physical value/charge");
            if (original.expires_at)
                check(decoded.replacement->expires_at ==
                          storage::WallTimePoint{std::chrono::microseconds{micros(original)}},
                      "deadline flooring");
            else
                check(!decoded.replacement->expires_at, "invented deadline");
        }
        if (lru)
            check(std::equal(f.order.begin(), f.order.end(), v.result()->lru_order().begin(),
                             v.result()->lru_order().end()),
                  "decoded LRU permutation");
    }
    const auto* summary = c.result();
    allocation_calls = 0;
    observe_allocations = true;
    const bool end_ok = c.next() == BaseEncodeStep::End && c.next() == BaseEncodeStep::End &&
                        c.result() == summary && c.complete() && c.bytes().empty();
    observe_allocations = false;
    check(end_ok && allocation_calls == 0 && f.fingerprint() == before,
          "idempotent allocation-free End/input preservation");
}
void positives() {
    scenario("independent CRC reference", [] { check(crc("123456789") == 0xe3069283U, "CRC"); });
    for (const bool lru : {false, true})
        for (const std::uint64_t sequence : {std::uint64_t{0}, std::uint64_t{17}})
            scenario("empty compaction with explicit context/LRU", [&] {
                Fixture f;
                f.ctx.captured_sequence = LogicalSequence{sequence};
                f.ctx.synchronized_source = {sequence, 96, 0, 0};
                f.bind();
                accept(f, lru);
            });
    scenario("frozen complete C/L vectors", [] {
        Fixture f;
        f.add(std::string("\0k", 2), storage::StringValue{std::string("\xff\0v", 3)});
        f.bind();
        accept(f, false);
        auto c = reference(f, false);
        check(c.size() == 244 && crc(c) == 0x155d9af1U && get32(c, 176) == 0x213707daU,
              "frozen C vector");
        f.add(std::string(1, '\xff'), storage::StringValue{"7"});
        f.bind();
        f.ctx.candidate_generation = GenerationId{std::uint64_t{4}};
        f.ctx.captured_sequence = LogicalSequence{std::uint64_t{2}};
        f.ctx.synchronized_source = {2, 200, 0xe91dbec0U, 0xa33c606bU};
        accept(f, true);
        const auto l = reference(f, true);
        check(l.size() == 316 && crc(l) == 0x6ddee862U && get32(l, 176) == 0xa646354bU,
              "frozen L vector");
        for (const auto& b : {c, l})
            for (std::size_t split = 0; split <= b.size(); ++split) {
                const auto ctx = b.size() == c.size() ? context() : f.ctx;
                BaseStreamValidator v(claims(b, ctx));
                check(v.feed(bytes(std::string_view{b}.substr(0, split))) ==
                              StreamStatus::NeedMore &&
                          v.feed(bytes(std::string_view{b}.substr(split))) ==
                              StreamStatus::NeedMore &&
                          v.finish() == StreamStatus::Complete,
                      "every two-chunk partition");
            }
    });
    scenario("all physical value types binary keys and canonical collections", [] {
        Fixture f;
        f.add("", storage::StringValue{"00017"});
        f.add(std::string("\0x", 2), storage::ListValue{{"", "a", "a", std::string("\xff\0", 2)}});
        f.add("h", storage::HashValue{{{"z", "last"}, {"", ""}, {std::string(1, '\xff'), "hi"}}});
        f.add(std::string(1, '\xff'), storage::SetValue{{"z", "", std::string("\0v", 2)}});
        f.bind();
        accept(f, false);
        accept(f, true);
        const auto original = reference(f, true);
        auto& hash = std::get<storage::HashValue>(f.entries[2].value).fields;
        hash.rehash(97);
        std::get<storage::SetValue>(f.entries[3].value).members.rehash(101);
        check(reference(f, true) == original, "inner canonical insertion/bucket order");
        accept(f, true);
    });
    for (const std::int64_t ticks : {std::int64_t{-1001}, std::int64_t{-1}, std::int64_t{0},
                                     std::int64_t{1}, std::int64_t{1001}, INT64_MAX})
        scenario("physical deadline native ticks and microsecond floor", [&] {
            Fixture f;
            f.add("ttl", storage::StringValue{"retained"},
                  storage::WallTimePoint{storage::WallTimePoint::duration{ticks}});
            f.bind();
            accept(f, true);
        });
    scenario("maximum candidate and zero CRC are valid claims", [] {
        Fixture f;
        f.ctx.candidate_generation = GenerationId{UINT64_MAX};
        f.ctx.synchronized_source = {1, 144, 0, 0};
        f.bind();
        accept(f, false);
    });
    for (const std::size_t k : {0U, 1U, 7U, 8U, 9U})
        scenario("permutation bitmap ceil boundaries", [&] {
            Fixture f;
            for (std::size_t i = 0; i < k; ++i)
                f.add(std::string(1, static_cast<char>('a' + i)), storage::StringValue{""});
            f.bind();
            auto budget = limits();
            budget.max_permutation_bytes = k / 8 + static_cast<std::size_t>(k % 8 != 0);
            auto c = BaseEncodingCursor::begin(f.ctx, f.input(true), budget);
            check(collect(c) == reference(f, true), "exact bitmap budget");
            if (k) {
                --budget.max_permutation_bytes;
                auto bad = BaseEncodingCursor::begin(f.ctx, f.input(true), budget);
                terminal(bad, BaseEncodeError::ResourceLimit);
            }
        });
}
void rejection(Fixture& f, BaseInputView input, BaseEncodeLimits budget, BaseEncodeError error) {
    const auto before = f.fingerprint();
    auto c = BaseEncodingCursor::begin(f.ctx, input, budget);
    terminal(c, error);
    check(c.bytes_emitted() == 0 && f.fingerprint() == before, "rejection emitted/mutated input");
}
void negatives() {
    scenario("invalid context variants and exhaustion", [] {
        for (unsigned which = 0; which < 8; ++which) {
            Fixture f;
            f.bind();
            switch (which) {
            case 0:
                f.ctx.candidate_generation = f.ctx.selected_generation;
                break;
            case 1:
                f.ctx.selected_generation = GenerationId{UINT64_MAX};
                break;
            case 2:
                f.ctx.captured_sequence = LogicalSequence{UINT64_MAX};
                break;
            case 3:
                f.ctx.synchronized_source.sequence = 9;
                break;
            case 4:
                f.ctx.synchronized_source.offset = 95;
                break;
            case 5:
                f.ctx.synchronized_source.offset = UINT64_MAX;
                break;
            case 6:
                f.ctx.synchronized_source.offset = 96;
                break;
            case 7:
                f.ctx.captured_sequence = LogicalSequence{std::uint64_t{0}};
                f.ctx.synchronized_source.sequence = 0;
                break;
            }
            rejection(f, f.input(false), limits(), BaseEncodeError::InvalidContext);
        }
        bool invalid_store = false, invalid_generation = false;
        try {
            (void)StoreId{StoreBytes{}};
        } catch (const FormatError&) {
            invalid_store = true;
        }
        try {
            (void)GenerationId{std::uint64_t{0}};
        } catch (const FormatError&) {
            invalid_generation = true;
        }
        check(invalid_store && invalid_generation, "strong identity constructors");
    });
    scenario("budget shape and deterministic context precedence", [] {
        for (unsigned which = 0; which < 5; ++which) {
            Fixture f;
            f.bind();
            auto budget = limits();
            if (which == 0)
                budget.max_bytes = 0;
            if (which == 1)
                budget.max_bytes = 203;
            if (which == 2)
                budget.max_bytes = UINT64_MAX;
            if (which == 3)
                budget.max_frame_payload = 3;
            if (which == 4)
                budget.max_frame_payload = MaxKeyPayload + 1;
            f.ctx.candidate_generation = f.ctx.selected_generation;
            rejection(f, f.input(false), budget, BaseEncodeError::ResourceLimit);
        }
        Fixture f;
        f.bind();
        auto budget = limits();
        budget.max_bytes = 204;
        budget.max_frame_payload = 4;
        auto c = BaseEncodingCursor::begin(f.ctx, f.input(false), budget);
        check(collect(c).size() == 204, "minimum complete extent");
    });
    scenario("null duplicate and unsigned out-of-order keys", [] {
        Fixture f;
        f.add("a", storage::StringValue{""});
        f.add("b", storage::StringValue{""});
        f.bind();
        f.views[0].entry = nullptr;
        rejection(f, f.input(false), limits(), BaseEncodeError::InvalidInput);
        f.bind();
        f.views[1].key = f.views[0].key;
        rejection(f, f.input(false), limits(), BaseEncodeError::KeyOrder);
        f.bind();
        std::swap(f.views[0], f.views[1]);
        rejection(f, f.input(false), limits(), BaseEncodeError::KeyOrder);
    });
    scenario("empty list hash and set reject before copy", [] {
        for (unsigned which = 0; which < 3; ++which) {
            Fixture f;
            storage::Value value = storage::ListValue{};
            if (which == 1)
                value = storage::HashValue{};
            if (which == 2)
                value = storage::SetValue{};
            f.add("key", std::move(value));
            f.bind();
            rejection(f, f.input(false), limits(), BaseEncodeError::EntryEncoding);
        }
    });
    scenario("entry extent charge and exact budget edges", [] {
        Fixture f;
        f.add("a", storage::StringValue{"v"});
        f.bind();
        const auto expected = reference(f, false);
        auto b = limits();
        b.max_bytes = expected.size();
        b.max_entries = 1;
        b.max_logical_charge = 66;
        b.max_frame_payload = 21;
        auto c = BaseEncodingCursor::begin(f.ctx, f.input(false), b);
        check(collect(c) == expected, "exact entry budgets");
        auto bad = b;
        --bad.max_bytes;
        rejection(f, f.input(false), bad, BaseEncodeError::ResourceLimit);
        bad = b;
        --bad.max_entries;
        rejection(f, f.input(false), bad, BaseEncodeError::ResourceLimit);
        bad = b;
        --bad.max_logical_charge;
        rejection(f, f.input(false), bad, BaseEncodeError::ResourceLimit);
        bad = b;
        --bad.max_frame_payload;
        rejection(f, f.input(false), bad, BaseEncodeError::ResourceLimit);
    });
    scenario("missing duplicate out-of-range and disabled LRU", [] {
        Fixture f;
        f.add("a", storage::StringValue{""});
        f.add("b", storage::StringValue{""});
        f.bind();
        rejection(f, {f.views, false, f.order}, limits(), BaseEncodeError::Permutation);
        f.order.pop_back();
        rejection(f, f.input(true), limits(), BaseEncodeError::Permutation);
        f.bind();
        f.order[1] = f.order[0];
        rejection(f, f.input(true), limits(), BaseEncodeError::Permutation);
        f.bind();
        f.order[0] = f.views.size();
        rejection(f, f.input(true), limits(), BaseEncodeError::Permutation);
    });
    scenario("oversized key value item count and frame", [] {
        Fixture f;
        f.add(std::string(MaxLruPayload + 1, 'k'), storage::StringValue{""});
        f.bind();
        rejection(f, f.input(false), limits(), BaseEncodeError::EntryEncoding);
        f.keys[0] = "k";
        f.entries[0].value = storage::StringValue{std::string(MaxLruPayload + 1, 'v')};
        f.bind();
        rejection(f, f.input(false), limits(), BaseEncodeError::EntryEncoding);
        storage::ListValue list;
        list.elements.resize(1'048'577);
        f.entries[0].value = std::move(list);
        f.bind();
        rejection(f, f.input(false), limits(), BaseEncodeError::EntryEncoding);
        storage::ListValue wide;
        for (unsigned i = 0; i < 9; ++i)
            wide.elements.emplace_back(MaxLruPayload, 'w');
        f.entries[0].value = std::move(wide);
        f.bind();
        rejection(f, f.input(false), limits(), BaseEncodeError::EntryEncoding);
    });
    scenario("minimum native deadline flooring rejects identically to codec", [] {
        Fixture f;
        f.add("ttl", storage::StringValue{""},
              storage::WallTimePoint{storage::WallTimePoint::duration{INT64_MIN}});
        f.bind();
        using D = storage::WallTimePoint::duration;
        constexpr auto scale = D::period::den / 1'000'000;
        if constexpr (scale > 1) {
            rejection(f, f.input(false), limits(), BaseEncodeError::EntryEncoding);
            bool rejected = false;
            try {
                (void)encode_key_frame({f.keys[0], f.entries[0]});
            } catch (const persistence::PersistenceError&) {
                rejected = true;
            }
            check(rejected, "native deadline differential rejection");
        } else {
            accept(f, false);
        }
    });
}
void moves() {
    scenario("default moved-from and no-allocation lifecycle", [] {
        BaseEncodingCursor empty;
        terminal(empty, BaseEncodeError::Closed);
        Fixture f;
        f.add("sso", storage::StringValue{""});
        f.bind();
        for (const unsigned chunks : {0U, 1U, 2U, 3U}) {
            auto c = BaseEncodingCursor::begin(f.ctx, f.input(false), limits());
            for (unsigned i = 0; i < chunks; ++i)
                check(c.next() == BaseEncodeStep::Chunk, "move setup chunk");
            const auto offered = text(c.bytes());
            const auto emitted = c.bytes_emitted();
            allocation_calls = armed = 0;
            observe_allocations = true;
            BaseEncodingCursor moved(std::move(c));
            BaseEncodingCursor target;
            target = std::move(moved);
            auto* alias = &target;
            target = std::move(*alias);
            const bool noalloc_state =
                target.bytes_emitted() == emitted && target.error().code == BaseEncodeError::None;
            observe_allocations = false;
            check(noalloc_state && allocation_calls == 0 && same_bytes(target.bytes(), offered),
                  "move transfer/SSO current view");
            terminal(c, BaseEncodeError::Closed);
            terminal(moved, BaseEncodeError::Closed);
            (void)collect(target);
            check(target.complete(), "moved completion");
            allocation_calls = 0;
            observe_allocations = true;
            BaseEncodingCursor done(std::move(target));
            const bool complete_move =
                done.complete() && done.result() && done.next() == BaseEncodeStep::End;
            observe_allocations = false;
            check(complete_move && allocation_calls == 0, "complete summary move");
        }
    });
    scenario("move replacement of active complete and failed pure targets", [] {
        Fixture f;
        f.add("heap", storage::StringValue{std::string(200, 'x')});
        f.bind();
        const auto before = f.fingerprint();
        for (unsigned phase = 0; phase < 4; ++phase) {
            auto target = BaseEncodingCursor::begin(f.ctx, f.input(true), limits());
            if (phase == 1) {
                check(target.next() == BaseEncodeStep::Chunk, "active target");
            }
            if (phase == 2) {
                (void)collect(target);
            }
            if (phase == 3) {
                auto bad = limits();
                bad.max_bytes = 0;
                target = BaseEncodingCursor::begin(f.ctx, f.input(true), bad);
            }
            auto incoming = BaseEncodingCursor::begin(f.ctx, f.input(true), limits());
            check(incoming.next() == BaseEncodeStep::Chunk &&
                      incoming.next() == BaseEncodeStep::Chunk,
                  "heap move setup");
            const auto offered = text(incoming.bytes());
            allocation_calls = armed = 0;
            observe_allocations = true;
            target = std::move(incoming);
            observe_allocations = false;
            check(allocation_calls == 0 && same_bytes(target.bytes(), offered), "heap transfer");
            terminal(incoming, BaseEncodeError::Closed);
            (void)collect(target);
            check(target.complete(), "replacement completion");
        }
        check(f.fingerprint() == before, "move changed input");
        const auto baseline = live_allocations;
        {
            auto early = BaseEncodingCursor::begin(f.ctx, f.input(true), limits());
            check(early.next() == BaseEncodeStep::Chunk && early.next() == BaseEncodeStep::Chunk,
                  "early destruction setup");
            allocation_calls = 0;
            observe_allocations = true;
        }
        observe_allocations = false;
        check(allocation_calls == 0 && live_allocations == baseline, "destructor leak/allocation");
    });
}
void corruptions() {
    scenario("truncation extra bytes and malformed grammar oracle", [] {
        Fixture f;
        f.add("a", storage::StringValue{"v"});
        f.bind();
        const auto b = reference(f, true);
        const auto reject = [&](std::string_view bad, BaseReferenceFields expected) {
            BaseStreamValidator validator(expected);
            (void)validator.feed(bytes(bad));
            check(validator.finish() == StreamStatus::Error && !validator.result(),
                  "corruption accepted by existing validator");
        };
        for (std::size_t cut = 0; cut < b.size(); ++cut) {
            const auto bad = std::string_view{b}.substr(0, cut);
            reject(bad, claims(b, f.ctx));
        }
        for (const auto at : {0U, 8U, 12U, 16U, 20U, 24U, 40U, 48U, 56U, 64U, 72U, 80U, 84U, 112U,
                              176U, 180U, 184U, 188U, 200U}) {
            auto bad = b;
            bad[at] ^= 1;
            reject(bad, claims(bad, f.ctx));
        }
        for (const auto& suffix : {std::string{"x"}, frame("DONE", be(0, 4)),
                                   key_frame("x", f.entries[0]), frame("LRU1", "x")}) {
            const auto bad = b + suffix;
            reject(bad, claims(bad, f.ctx));
        }
        // Reseal header/body/footer and adjust reference to reach semantic checks.
        for (unsigned which = 0; which < 3; ++which) {
            auto prefix = b.substr(0, b.size() - 20);
            if (which == 0)
                put(prefix, 64, 999, 8); // physical charge mismatch
            if (which == 1)
                put(prefix, 56, 2, 8); // impossible count/grammar
            if (which == 2)
                put(prefix, 72, 0, 8); // LRU flag/count conflict
            put(prefix, 176, crc(std::string_view{prefix}.substr(0, 176)), 4);
            const auto bad = prefix + frame("DONE", be(crc(prefix), 4));
            reject(bad, claims(bad, f.ctx));
        }
        auto foreign = claims(b, f.ctx);
        foreign.generation = 9;
        reject(b, foreign);
        foreign = claims(b, f.ctx);
        foreign.store[0] = std::byte{42};
        reject(b, foreign);
        foreign = claims(b, f.ctx);
        ++foreign.sequence;
        reject(b, foreign);
        foreign = claims(b, f.ctx);
        foreign.file_crc ^= 1U;
        reject(b, foreign);
    });
}
struct RunResult {
    BaseEncodeFailure failure;
    std::size_t calls, hits, largest, peak, emitted;
    bool finished, no_output, sticky;
};
RunResult run_allocations(Fixture& f, bool lru, std::size_t ordinal) {
    allocation_calls = reached = largest_request = 0;
    peak_live = live_bytes;
    const auto starting_bytes = live_bytes;
    operation = 0;
    armed = ordinal;
    RunResult result{};
    observe_allocations = true;
    {
        auto c = BaseEncodingCursor::begin(f.ctx, f.input(lru), limits());
        if (c.error().code == BaseEncodeError::None) {
            for (std::size_t n = 1;; ++n) {
                operation = n;
                const auto step = c.next();
                if (step != BaseEncodeStep::Chunk)
                    break;
            }
        }
        result.failure = c.error();
        result.finished = c.complete();
        result.no_output = c.bytes().empty() && !c.result();
        result.emitted = static_cast<std::size_t>(c.bytes_emitted());
        result.sticky = true;
        const auto calls = allocation_calls;
        if (!result.finished)
            for (unsigned i = 0; i < 3; ++i) {
                const auto step = c.next();
                const auto failure = c.error();
                result.sticky = result.sticky && step == BaseEncodeStep::Error &&
                                failure.code == result.failure.code &&
                                failure.fixed_format == result.failure.fixed_format &&
                                failure.frame_format == result.failure.frame_format &&
                                c.bytes().empty() && !c.complete() && !c.result();
            }
        result.sticky = result.sticky && calls == allocation_calls;
    }
    observe_allocations = false;
    result.calls = allocation_calls;
    result.hits = reached;
    result.largest = largest_request;
    result.peak = peak_live - starting_bytes;
    armed = 0;
    return result;
}
void allocation_campaign() {
    std::size_t planned = 0, validated = 0, begin_points = 0, next_points = 0;
    for (unsigned variant = 0; variant < 8; ++variant) {
        Fixture f;
        const bool lru = (variant % 2) != 0;
        if (variant >= 2) {
            storage::Value value = storage::StringValue{std::string(257, 'v')};
            if (variant == 3)
                value = storage::ListValue{{"a", std::string(89, 'l'), "a"}};
            if (variant == 4)
                value = storage::HashValue{{{"z", std::string(99, 'h')}, {"a", "1"}}};
            if (variant == 5)
                value = storage::SetValue{{"z", std::string(128, 's'), "a"}};
            f.add("binary\0key", std::move(value));
            if (variant >= 6) {
                f.add("list", storage::ListValue{{std::string(129, 'l'), "a"}},
                      storage::WallTimePoint{storage::WallTimePoint::duration{-1001}});
                f.add("zz", storage::StringValue{std::string(513, 'z')});
            }
        }
        f.bind();
        const auto before = f.fingerprint();
        const auto baseline = live_allocations, baseline_bytes = live_bytes;
        const auto healthy = run_allocations(f, lru, 0);
        check(healthy.finished && healthy.failure.code == BaseEncodeError::None &&
                  healthy.hits == 0 && healthy.sticky && live_allocations == baseline &&
                  live_bytes == baseline_bytes,
              "healthy allocation trace/leak");
        const auto trace = allocation_trace;
        std::cout << "ALLOC_TRACE variant=" << variant << " calls=" << healthy.calls
                  << " largest_request=" << healthy.largest
                  << " peak_extra_requested=" << healthy.peak << " retained_after_cleanup=0\n";
        for (std::size_t i = 0; i < healthy.calls; ++i) {
            ++planned;
            if (trace[i].operation == 0)
                ++begin_points;
            else
                ++next_points;
            std::cout << "ALLOC_PLAN variant=" << variant << " ordinal=" << i + 1
                      << " operation=" << trace[i].operation << " bytes=" << trace[i].size << '\n';
            const auto actual = run_allocations(f, lru, i + 1);
            check(actual.hits == 1 && actual.calls >= i + 1 && !actual.finished &&
                      actual.no_output && actual.failure.code == BaseEncodeError::ResourceLimit &&
                      actual.sticky && allocation_trace[i].size == trace[i].size &&
                      allocation_trace[i].operation == trace[i].operation &&
                      live_allocations == baseline && live_bytes == baseline_bytes,
                  "allocation fault not reached/translated/cleaned exactly");
            check(f.fingerprint() == before, "allocation fault mutated input");
            ++validated;
            ++injections;
            ++scenarios;
            std::cout << "ALLOC_INJECTION variant=" << variant << " planned=" << i + 1
                      << " reached=1 validated=1 error=ResourceLimit sticky=1 output=none"
                         " input=unchanged live_delta=0 emitted_before_failure="
                      << actual.emitted << '\n';
        }
        ++scenarios;
    }
    check(planned == validated && planned == injections, "allocation campaign accountability");
    std::cout << "ALLOC_CAMPAIGN variants=8 before_begin_points=" << begin_points
              << " next_points=" << next_points << " planned=" << planned
              << " reached=" << injections << " validated=" << validated
              << " excluded=0 filesystem_before_call_after=0\n";
}
void large_stream() {
    scenario("nine 8MiB entries stream beyond aggregate 64MiB", [] {
        Fixture f;
        for (unsigned i = 0; i < 9; ++i)
            f.add(std::string(1, static_cast<char>('a' + i)),
                  storage::StringValue{std::string(MaxLruPayload, static_cast<char>('0' + i))});
        f.bind();
        const auto h = header(f, false);
        auto running = crc(h);
        std::uint64_t extent = h.size();
        for (const auto& v : f.views) {
            const auto reference_frame = key_frame(v.key, *v.entry);
            running = crc(reference_frame, running);
            extent += reference_frame.size();
        }
        const auto done = frame("DONE", be(running, 4));
        running = crc(done, running);
        extent += done.size();
        check(extent > MaxKeyPayload, "large fixture not greater than 64MiB");
        BaseReferenceFields expected{1,
                                     f.ctx.store.bytes(),
                                     f.ctx.candidate_generation.value(),
                                     f.ctx.captured_sequence.value(),
                                     extent,
                                     running,
                                     get32(h, 176)};
        BaseStreamValidator validator(expected);
        auto c = BaseEncodingCursor::begin(f.ctx, f.input(false), limits());
        std::size_t chunk = 0, maximum = 0;
        std::uint64_t observed = 0;
        std::size_t peak_cursor = 0;
        for (;;) {
            allocation_calls = armed = 0;
            peak_live = live_bytes;
            const auto baseline = live_bytes;
            observe_allocations = true;
            const auto step = c.next();
            observe_allocations = false;
            peak_cursor = std::max(peak_cursor, peak_live - baseline);
            check(step != BaseEncodeStep::Error, "large stream failed");
            if (step == BaseEncodeStep::End)
                break;
            const auto independent =
                chunk == 0 ? h
                : chunk <= f.views.size()
                    ? key_frame(f.views[chunk - 1].key, *f.views[chunk - 1].entry)
                    : done;
            check(same_bytes(c.bytes(), independent), "large independent frame differs");
            maximum = std::max(maximum, c.bytes().size());
            observed += c.bytes().size();
            auto input = c.bytes();
            while (!input.empty()) {
                const auto n = std::min(input.size(), std::size_t{65536});
                check(validator.feed(input.first(n)) == StreamStatus::NeedMore,
                      "large decoder feed");
                input = input.subspan(n);
            }
            ++chunk;
        }
        check(chunk == 11 && observed == extent && c.result() &&
                  c.result()->identity().file_crc.value() == running &&
                  c.result()->identity().byte_length == extent &&
                  validator.finish() == StreamStatus::Complete && validator.result() &&
                  validator.result()->entries().size() == 9 && maximum < extent,
              "large full EOF/identity/count");
        for (std::size_t i = 0; i < f.entries.size(); ++i)
            check(validator.result()->entries()[i].replacement->value == f.entries[i].value &&
                      f.entries[i].accounted_charge == 999,
                  "large physical/input preservation");
        std::cout << "LARGE_STREAM bytes=" << extent << " chunks=" << chunk
                  << " max_chunk=" << maximum
                  << " peak_cursor_operation_extra_requested=" << peak_cursor
                  << " whole_base_buffer=0 independent_full_base_copy=0\n";
    });
    scenario("exact maximum KEY payload accepted then one byte over rejected", [] {
        Fixture f;
        storage::ListValue value;
        for (unsigned i = 0; i < 7; ++i)
            value.elements.emplace_back(MaxLruPayload, 'a');
        value.elements.emplace_back(MaxLruPayload - 52, 'b');
        f.add("a", std::move(value));
        f.bind();
        auto c = BaseEncodingCursor::begin(f.ctx, f.input(false), limits());
        check(c.error().code == BaseEncodeError::None && c.next() == BaseEncodeStep::Chunk &&
                  c.next() == BaseEncodeStep::Chunk && c.bytes().size() == MaxKeyPayload + 16,
              "maximum payload differential size");
        const auto reference_frame = key_frame(f.keys[0], f.entries[0]);
        check(same_bytes(c.bytes(), reference_frame), "maximum payload independent bytes");
        check(c.next() == BaseEncodeStep::Chunk && c.bytes().size() == 20 &&
                  c.next() == BaseEncodeStep::End,
              "maximum payload finish");
        c = BaseEncodingCursor{}; // Release the borrow before changing the fixture.
        std::get<storage::ListValue>(f.entries[0].value).elements.back().push_back('x');
        auto bad = BaseEncodingCursor::begin(f.ctx, f.input(false), limits());
        terminal(bad, BaseEncodeError::EntryEncoding);
    });
}
void scalar_return_campaign() {
    for (unsigned which = 0; which < 13; ++which) {
        scenario("scalar rejection retains first error without new error-path allocations", [&] {
            Fixture f;
            f.add("a", storage::StringValue{"a"});
            f.add("b", storage::StringValue{"b"});
            f.bind();
            auto budget = limits();
            auto expected = BaseEncodeError::ResourceLimit;
            switch (which) {
            case 0:
                budget.max_bytes = 203;
                break;
            case 1:
                f.ctx.captured_sequence = LogicalSequence{UINT64_MAX};
                expected = BaseEncodeError::InvalidContext;
                break;
            case 2:
                f.views[1].key = f.views[0].key;
                expected = BaseEncodeError::KeyOrder;
                break;
            case 3:
                std::swap(f.views[0], f.views[1]);
                expected = BaseEncodeError::KeyOrder;
                break;
            case 4:
                f.views[0].entry = nullptr;
                expected = BaseEncodeError::InvalidInput;
                break;
            case 5:
                f.order[1] = f.order[0];
                expected = BaseEncodeError::Permutation;
                break;
            case 6:
                budget.max_entries = 1;
                break;
            case 7:
                budget.max_logical_charge = 0;
                break;
            case 8:
                budget.max_bytes = 204;
                break;
            case 9:
                budget.max_frame_payload = 19;
                break;
            case 10:
                f.entries[0].value = storage::ListValue{};
                expected = BaseEncodeError::EntryEncoding;
                break;
            case 11:
                ++f.ctx.synchronized_source.sequence;
                expected = BaseEncodeError::InvalidContext;
                break;
            case 12:
                budget.max_permutation_bytes = 0;
                break;
            }
            const auto before = f.fingerprint();
            const auto baseline_count = live_allocations, baseline_bytes = live_bytes;
            allocation_calls = armed = 0;
            bool correct = true;
            std::size_t before_error = 0;
            observe_allocations = true;
            {
                auto cursor = BaseEncodingCursor::begin(f.ctx, f.input(true), budget);
                const auto first = cursor.error();
                before_error = allocation_calls;
                correct = first.code == expected && !first.fixed_format && !first.frame_format &&
                          cursor.bytes_emitted() == 0;
                for (unsigned repeat = 0; repeat < 4; ++repeat) {
                    const auto step = cursor.next();
                    const auto failure = cursor.error();
                    correct =
                        correct && step == BaseEncodeStep::Error && failure.code == first.code &&
                        failure.fixed_format == first.fixed_format &&
                        failure.frame_format == first.frame_format && cursor.bytes().empty() &&
                        !cursor.complete() && !cursor.result() && cursor.bytes_emitted() == 0;
                }
            }
            observe_allocations = false;
            check(correct && allocation_calls == before_error &&
                      live_allocations == baseline_count && live_bytes == baseline_bytes &&
                      f.fingerprint() == before,
                  "scalar failure/terminal getters/input/leak");
            std::cout
                << "SCALAR_RETURN case=" << which << " error=" << static_cast<int>(expected)
                << " new_calls_before_rejection=" << before_error
                << " new_calls_after_rejection=0 first_error=stable output=none input=unchanged"
                   " ABI_allocation=separate_external_observer\n";
        });
    }
}
} // namespace
int main() {
    const auto allocations = live_allocations, memory = live_bytes;
    try {
        positives();
        negatives();
        moves();
        corruptions();
        allocation_campaign();
        large_stream();
        scalar_return_campaign();
        check(live_allocations == allocations && live_bytes == memory, "process allocation leak");
        std::cout << "PASS Base encoding: " << scenarios << " executed scenarios; " << injections
                  << " planned/reached/validated allocation injections; filesystem hooks=0;"
                     " no cursor I/O or publication; source input preserved\n";
        return 0;
    } catch (const std::exception& e) {
        observe_allocations = false;
        armed = 0;
        std::cerr << "FAIL Base encoding after " << scenarios << " scenarios: " << e.what() << '\n';
        return 1;
    }
}
