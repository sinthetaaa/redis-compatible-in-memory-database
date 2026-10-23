#include "redisdb/storage/MemoryAccounting.hpp"

#include <cstddef>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace ma = redisdb::storage::memory_accounting;

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename F> void expect_failure(F&& operation, const char* message) {
    try {
        operation();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void test_replacement_and_removal() {
    require(ma::replacement_total(0, 0, 64) == 64, "first entry insert charge");
    require(ma::replacement_total(100, 50, 20) == 70, "replacement shrink");
    require(ma::replacement_total(100, 50, 90) == 140, "replacement grow");
    require(ma::replacement_total(100, 50, 50) == 100, "same-size no-op");
    require(ma::replacement_total(0, 0, 0) == 0, "empty total");
    require(ma::removal_total(100, 60) == 40, "remove existing charge");
    require(ma::removal_total(100, 0) == 100, "remove zero charge");
    require(ma::removal_total(100, 100) == 0, "remove final entry");
}

void test_checked_boundaries() {
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    require(ma::replacement_total(max, max, max) == max, "avoid spurious intermediate overflow");
    require(ma::replacement_total(max, max, 0) == 0, "replace max with zero");
    require(ma::replacement_total(max, 10, 10) == max, "same total at max");
    expect_failure<std::overflow_error>([=] { (void)ma::replacement_total(max, 0, 1); },
                                        "growth overflow");
    expect_failure<std::overflow_error>([=] { (void)ma::replacement_total(max, 1, 2); },
                                        "growth overflow after debit");
    expect_failure<std::logic_error>([] { (void)ma::replacement_total(5, 6, 0); },
                                     "bad previous charge");
    expect_failure<std::logic_error>([] { (void)ma::removal_total(5, 6); }, "removal underflow");
}

void test_independent_replay_and_variant_sizes() {
    std::vector<std::size_t> charges(23, 0);
    std::size_t total = 0;
    std::minstd_rand rng(20261024);
    for (int i = 0; i < 5000; ++i) {
        auto index = static_cast<std::size_t>(rng() % charges.size());
        auto candidate = static_cast<std::size_t>(rng() % 1024);
        if (i % 7 == 0) {
            candidate = 0;
        }
        const auto old = charges[index];
        const auto planned = ma::replacement_total(total, old, candidate);
        charges[index] = candidate;
        total = planned;
        const auto reference = std::accumulate(charges.begin(), charges.end(), std::size_t{0});
        require(total == reference, "ledger drift in independently summed replay");
    }
    for (auto& charge : charges) {
        total = ma::removal_total(total, charge);
        charge = 0;
    }
    require(total == 0, "ledger never returned to zero");

    using redisdb::storage::Entry;
    using redisdb::storage::StringValue;
    using redisdb::storage::Value;
    const Entry binary{Value{StringValue{std::string("a\0b", 3)}}};
    const auto old_charge = ma::entry_charge(std::string("x\0", 2), binary);
    require(old_charge == 64 + 2 + 3, "independent binary entry fixture");
    const auto with_one_more = ma::replacement_total(old_charge, old_charge, old_charge + 1);
    require(with_one_more == old_charge + 1, "binary entry replacement delta");
}
} // namespace

int main() {
    try {
        test_replacement_and_removal();
        test_checked_boundaries();
        test_independent_replay_and_variant_sizes();
        std::cout << "PASS: checked ledger transitions\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
