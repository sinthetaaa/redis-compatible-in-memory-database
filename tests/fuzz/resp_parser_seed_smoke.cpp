#include "RespParserFuzzChecks.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::filesystem::path> collect_seeds(const std::filesystem::path& directory) {
    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error("Seed corpus directory does not exist");
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            throw std::runtime_error("Unexpected non-file entry in seed corpus");
        }
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.size() < 16) {
        throw std::runtime_error("Expected at least 16 initial Phase 2G.1A seeds");
    }
    return files;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("Usage: redisdb-resp-parser-seed-smoke <seed-directory>");
        }

        const auto files = collect_seeds(argv[1]);
        for (const auto& path : files) {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                throw std::runtime_error("Could not open seed file: " + path.string());
            }
            const std::string bytes((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
            if (input.bad()) {
                throw std::runtime_error("Could not read seed file: " + path.string());
            }
            if (bytes.size() > redisdb::fuzz::kMaxFuzzInputBytes) {
                throw std::runtime_error("Oversized seed: " + path.string());
            }
            std::cout << "REPLAY " << path.filename().string() << " (" << bytes.size() << " bytes)"
                      << std::endl;
            redisdb::fuzz::fuzz_one(std::string_view(bytes.data(), bytes.size()));
        }

        std::cout << "PASS: " << files.size() << " deterministic RESP2 seeds replayed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RESP2 seed runner failed: " << error.what() << '\n';
        return 1;
    }
}
