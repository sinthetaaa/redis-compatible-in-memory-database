#include "redisdb/storage/Database.hpp"

#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using redisdb::storage::Database;
using redisdb::storage::IntegerMutationError;
using redisdb::storage::parse_integer64;

namespace {

void check(bool ok, const char* why) {
    if (!ok) {
        throw std::runtime_error(why);
    }
}

struct Reference {
    bool overflow = false;
    std::int64_t value = 0;
};

// Independent two's-complement sign-transition oracle. Every arithmetic
// operation happens in the unsigned domain; no overflowing signed addition,
// subtraction, or negation is evaluated by this reference model.
Reference reference(std::int64_t current, std::int64_t operand, bool subtract) {
    const auto u_current = static_cast<std::uint64_t>(current);
    const auto u_operand = static_cast<std::uint64_t>(operand);
    const std::uint64_t u_result = subtract ? u_current - u_operand : u_current + u_operand;
    const auto result = std::bit_cast<std::int64_t>(u_result);
    const bool bad = subtract ? ((current >= 0 && operand < 0 && result < 0) ||
                                 (current < 0 && operand > 0 && result >= 0))
                              : ((current >= 0 && operand >= 0 && result < 0) ||
                                 (current < 0 && operand < 0 && result >= 0));
    return {bad, result};
}

void parsing_corpus() {
    const std::vector<std::int64_t> boundaries = {std::numeric_limits<std::int64_t>::min(),
                                                  std::numeric_limits<std::int64_t>::min() + 1,
                                                  -1000000000,
                                                  -1,
                                                  0,
                                                  1,
                                                  1000000000,
                                                  std::numeric_limits<std::int64_t>::max() - 1,
                                                  std::numeric_limits<std::int64_t>::max()};
    for (const auto value : boundaries) {
        const auto repr = std::to_string(value);
        check(parse_integer64(repr) == value, "canonical boundary parse failed");
    }
    for (std::int64_t n = -4096; n <= 4096; ++n) {
        check(parse_integer64(std::to_string(n)) == n, "canonical integer corpus failed");
    }
    const std::vector<std::string> malformed = {"",
                                                "-",
                                                "+",
                                                "+0",
                                                "+1",
                                                "-0",
                                                "00",
                                                "01",
                                                "-00",
                                                "-01",
                                                " 0",
                                                "0 ",
                                                " 1",
                                                "1 ",
                                                "\t1",
                                                "1\n",
                                                "1.0",
                                                "1e0",
                                                "0x1",
                                                "--1",
                                                "1-",
                                                "9223372036854775808",
                                                "-9223372036854775809",
                                                "18446744073709551615",
                                                "-18446744073709551616",
                                                std::string("1\0", 2),
                                                std::string("\0", 1),
                                                std::string("1\xff", 2),
                                                std::string("\xef\xbc\x91", 3),
                                                std::string(4096, '9')};
    for (const auto& value : malformed) {
        check(!parse_integer64(value).has_value(), "invalid integer was accepted");
    }
}

void boundary_matrix() {
    constexpr auto Min = std::numeric_limits<std::int64_t>::min();
    constexpr auto Max = std::numeric_limits<std::int64_t>::max();
    const std::vector<std::int64_t> values = {Min, Min + 1, Min + 2, -1024,   -2,      -1, 0,
                                              1,   2,       1024,    Max - 2, Max - 1, Max};
    const std::vector<std::int64_t> deltas = {Min, Min + 1, -1024, -2,      -1, 0,
                                              1,   2,       1024,  Max - 1, Max};
    Database db;
    const std::string key("border\0\xff", 8);
    for (const auto current : values) {
        for (const auto operand : deltas) {
            for (const bool subtract : {false, true}) {
                const std::string before = std::to_string(current);
                (void)db.set(key, before);
                const auto expected = reference(current, operand, subtract);
                const auto observed = db.adjust_integer(key, operand, subtract);
                if (expected.overflow) {
                    check(observed.error == IntegerMutationError::Overflow,
                          "boundary overflow missing");
                    check(db.get(key) == before, "overflow modified original bytes");
                } else {
                    check(observed.error == IntegerMutationError::None,
                          "valid boundary arithmetic rejected");
                    check(observed.value == expected.value, "boundary arithmetic wrong result");
                    check(db.get(key) == std::to_string(expected.value),
                          "boundary arithmetic stored incorrect bytes");
                }
            }
        }
    }
    // A deterministic mixed-sign regression corpus backed by the same
    // independent unsigned-domain reference. Fixed seed makes CI repeatable.
    std::uint64_t state = 0xA5B3C671D29E4F08ULL;
    for (int iteration = 0; iteration < 3000; ++iteration) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const auto current = std::bit_cast<std::int64_t>(state);
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const auto operand = std::bit_cast<std::int64_t>(state);
        const bool subtract = (iteration % 2) != 0;
        (void)db.set(key, std::to_string(current));
        const auto expected = reference(current, operand, subtract);
        const auto actual = db.adjust_integer(key, operand, subtract);
        check((actual.error == IntegerMutationError::Overflow) == expected.overflow,
              "deterministic wide arithmetic overflow mismatch");
        if (expected.overflow) {
            check(db.get(key) == std::to_string(current), "wide overflow mutated storage");
        } else {
            check(actual.value == expected.value, "wide arithmetic incorrect");
            check(db.get(key) == std::to_string(expected.value), "wide arithmetic bytes wrong");
        }
    }
}

void failed_mutations_and_absence() {
    constexpr auto Min = std::numeric_limits<std::int64_t>::min();
    Database db;
    const std::string binary_key("numeric\0\xff", 9);
    check(!db.exists(binary_key), "binary key should initially be missing");
    const auto rejected = db.adjust_integer(binary_key, Min, true);
    check(rejected.error == IntegerMutationError::Overflow,
          "subtracting INT64_MIN from missing key must overflow");
    check(!db.exists(binary_key), "overflow created missing key");

    const std::vector<std::string> raw = {"-0",
                                          "001",
                                          "+10",
                                          "9223372036854775808",
                                          "-9223372036854775809",
                                          std::string("1\0b", 3),
                                          std::string(2048, '8')};
    for (const auto& bytes : raw) {
        (void)db.set(binary_key, bytes);
        const auto first = db.adjust_integer(binary_key, 1, false);
        const auto second = db.adjust_integer(binary_key, Min, true);
        check(first.error == IntegerMutationError::InvalidInteger &&
                  second.error == IntegerMutationError::InvalidInteger,
              "invalid bytes were not rejected");
        check(db.get(binary_key) == bytes, "invalid integer mutation changed storage");
    }
    db.clear();
    check(!db.exists(binary_key), "clear did not erase value");
    const auto created = db.adjust_integer(binary_key, Min, false);
    check(created.error == IntegerMutationError::None && created.value == Min,
          "INT64_MIN on absent key should be stored");
    check(db.get(binary_key) == std::to_string(Min), "binary key storage not canonical");
}

} // namespace

int main() {
    try {
        parsing_corpus();
        boundary_matrix();
        failed_mutations_and_absence();
        std::cout << "PASS: 4G 8201+ parses, full 64-bit grid, 3000 wide reference checks, "
                     "unchanged state\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
