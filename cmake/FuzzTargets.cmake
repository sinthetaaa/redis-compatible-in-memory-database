# Opt-in standalone sanitizer replay for Phase 2G.1A. The libFuzzer
# engine and its separate CI workflow are intentionally deferred to 2G.2.
if(NOT BUILD_TESTING)
    message(FATAL_ERROR "REDISDB_ENABLE_FUZZ_TARGETS requires BUILD_TESTING=ON")
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
    message(FATAL_ERROR "Phase 2G.1A sanitizer seed runner requires Clang")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_redisdb_fuzz_corpus "${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus")
set(_redisdb_fuzz_stamp "${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus.ready")

add_custom_command(
    OUTPUT "${_redisdb_fuzz_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_redisdb_fuzz_corpus}"
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/fuzz/generate_resp2_corpus.py"
            --output "${_redisdb_fuzz_corpus}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_redisdb_fuzz_stamp}"
    DEPENDS "${PROJECT_SOURCE_DIR}/tests/fuzz/generate_resp2_corpus.py"
    VERBATIM
)
add_custom_target(redisdb-resp-parser-fuzz-seeds DEPENDS "${_redisdb_fuzz_stamp}")

# Compile the real production parser and value implementation directly
# into this instrumented target. Do not link the uninstrumented core.
add_executable(
    redisdb-resp-parser-seed-smoke
    "${PROJECT_SOURCE_DIR}/tests/fuzz/resp_parser_seed_smoke.cpp"
    "${PROJECT_SOURCE_DIR}/tests/fuzz/resp_parser_fuzz_checks.cpp"
    "${PROJECT_SOURCE_DIR}/src/protocol/RespParser.cpp"
    "${PROJECT_SOURCE_DIR}/src/protocol/RespValue.cpp"
)

target_include_directories(redisdb-resp-parser-seed-smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/tests/fuzz"
)
target_compile_features(redisdb-resp-parser-seed-smoke PRIVATE cxx_std_20)
set_target_properties(redisdb-resp-parser-seed-smoke PROPERTIES CXX_EXTENSIONS OFF)
redisdb_enable_warnings(redisdb-resp-parser-seed-smoke)
# Apple Clang 17's ASan runtime hangs during shadow-memory startup on
# the audited macOS host. Use real UBSan instrumentation for the local
# deterministic seed runner; Linux must retain both ASan and UBSan.
# Neither branch changes production nor default Debug/Release targets.
if(APPLE)
    set(_redisdb_seed_sanitizers "undefined")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_redisdb_seed_sanitizers "address,undefined")
else()
    message(FATAL_ERROR "Unsupported sanitizer seed-runner platform")
endif()

target_compile_options(redisdb-resp-parser-seed-smoke PRIVATE
    "-fsanitize=${_redisdb_seed_sanitizers}"
    -fno-omit-frame-pointer
    -fno-sanitize-recover=all
)
target_link_options(redisdb-resp-parser-seed-smoke PRIVATE
    "-fsanitize=${_redisdb_seed_sanitizers}"
    -fno-sanitize-recover=all
)
add_dependencies(redisdb-resp-parser-seed-smoke redisdb-resp-parser-fuzz-seeds)

# Fail the CTest before running the C++ target if any seed, name, or
# sidecar SHA-256 manifest is missing, modified, or unexpected.
# The sidecar sits outside the corpus so libFuzzer never treats it as a seed.
add_test(
    NAME fuzz.resp_parser_corpus_smoke
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/fuzz/generate_resp2_corpus.py"
            --output "${_redisdb_fuzz_corpus}"
            --verify
            --runner "$<TARGET_FILE:redisdb-resp-parser-seed-smoke>"
)
set_tests_properties(fuzz.resp_parser_corpus_smoke PROPERTIES TIMEOUT 30)

# Phase 2G.2A: Linux-only, coverage-guided LLVM libFuzzer executable.
# Apple Clang's audited runtime lacks libFuzzer; retain the macOS
# standalone UBSan corpus runner without a new mandatory dependency.
# This target compiles real parser sources separately with coverage and
# sanitizers. Normal Debug/Release presets never include this file.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "Linux RESP2 libFuzzer target requires LLVM Clang")
    endif()

    add_executable(
        redisdb-resp-parser-libfuzzer
        "${PROJECT_SOURCE_DIR}/tests/fuzz/resp_parser_fuzz_entry.cpp"
        "${PROJECT_SOURCE_DIR}/tests/fuzz/resp_parser_fuzz_checks.cpp"
        "${PROJECT_SOURCE_DIR}/src/protocol/RespParser.cpp"
        "${PROJECT_SOURCE_DIR}/src/protocol/RespValue.cpp"
    )
    target_include_directories(redisdb-resp-parser-libfuzzer PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/tests/fuzz"
    )
    target_compile_features(redisdb-resp-parser-libfuzzer PRIVATE cxx_std_20)
    set_target_properties(redisdb-resp-parser-libfuzzer PROPERTIES CXX_EXTENSIONS OFF)
    redisdb_enable_warnings(redisdb-resp-parser-libfuzzer)
    target_compile_options(redisdb-resp-parser-libfuzzer PRIVATE
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options(redisdb-resp-parser-libfuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-sanitize-recover=all
    )
    add_dependencies(redisdb-resp-parser-libfuzzer redisdb-resp-parser-fuzz-seeds)
endif()
