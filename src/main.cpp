#include "redisdb/server/Version.hpp"

#include <iostream>
#include <string_view>

namespace {

void print_usage() {
    std::cout << "Redis-Compatible In-Memory Database Server\n"
              << "\n"
              << "Usage:\n"
              << "  redisdb-server --version\n"
              << "  redisdb-server --help\n"
              << "\n"
              << "Phase 0: build-system foundation.\n"
              << "TCP server functionality is not implemented yet.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view argument{argv[1]};

        if (argument == "--version") {
            std::cout << "redisdb-server " << redisdb::server::version() << '\n';

            return 0;
        }

        if (argument == "--help") {
            print_usage();
            return 0;
        }
    }

    std::cerr << "Invalid arguments. Use --help for usage.\n";

    return 2;
}
