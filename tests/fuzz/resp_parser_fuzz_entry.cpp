#include "RespParserFuzzChecks.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

// libFuzzer supplies main(). Preserve every byte, including embedded NULs.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > redisdb::fuzz::kMaxFuzzInputBytes) {
        return 0;
    }

    if (size == 0) {
        redisdb::fuzz::fuzz_one(std::string_view{});
        return 0;
    }

    if (data == nullptr) {
        return 0; // Defensive API boundary: libFuzzer does not pass this pair.
    }

    redisdb::fuzz::fuzz_one(std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
