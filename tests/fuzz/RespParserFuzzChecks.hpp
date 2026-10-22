#pragma once

#include <cstddef>
#include <string_view>

namespace redisdb::fuzz {

inline constexpr std::size_t kMaxFuzzInputBytes = 4096;

// Always-on parser invariants shared by the standalone corpus runner
// and a subsequent libFuzzer entry point. An invariant violation aborts.
void fuzz_one(std::string_view input);

} // namespace redisdb::fuzz
