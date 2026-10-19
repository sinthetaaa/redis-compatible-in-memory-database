#include "redisdb/server/Version.hpp"

#include <iostream>
#include <string_view>

int main() {
    constexpr std::string_view expected = "0.1.0-dev";

    if (redisdb::server::version() != expected) {
        std::cerr << "Version test failed.\n";
        return 1;
    }

    std::cout << "Version test passed.\n";
    return 0;
}
