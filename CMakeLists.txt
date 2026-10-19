cmake_minimum_required(VERSION 3.25)

project(
    RedisCompatibleDB
    VERSION 0.1.0
    LANGUAGES CXX
)

include(CTest)

function(redisdb_enable_warnings target_name)
    if(MSVC)
        target_compile_options(
            ${target_name}
            PRIVATE /W4
        )
    else()
        target_compile_options(
            ${target_name}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
        )
    endif()
endfunction()

add_library(
    redisdb-core STATIC
    src/server/Version.cpp
    src/net/UniqueFd.cpp
    src/net/TcpListener.cpp
)

target_compile_features(
    redisdb-core PUBLIC cxx_std_20
)

set_target_properties(
    redisdb-core
    PROPERTIES CXX_EXTENSIONS OFF
)

target_include_directories(
    redisdb-core
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)

redisdb_enable_warnings(redisdb-core)

add_executable(
    redisdb-server
    src/main.cpp
)

target_link_libraries(
    redisdb-server
    PRIVATE redisdb-core
)

redisdb_enable_warnings(redisdb-server)

if(BUILD_TESTING)
    add_executable(
        redisdb-tests
        tests/unit/version_test.cpp
    )

    target_link_libraries(
        redisdb-tests
        PRIVATE redisdb-core
    )

    redisdb_enable_warnings(redisdb-tests)

    add_test(
        NAME unit.version
        COMMAND redisdb-tests
    )

    add_test(
        NAME cli.version
        COMMAND redisdb-server --version
    )

    set_tests_properties(
        cli.version
        PROPERTIES
            PASS_REGULAR_EXPRESSION
                "redisdb-server 0.1.0-dev"
    )

    add_executable(
        redisdb-fd-tests
        tests/unit/unique_fd_test.cpp
    )

    target_link_libraries(
        redisdb-fd-tests
        PRIVATE redisdb-core
    )

    redisdb_enable_warnings(redisdb-fd-tests)

    add_test(
        NAME unit.unique_fd
        COMMAND redisdb-fd-tests
    )

    set_tests_properties(
        unit.unique_fd
        PROPERTIES TIMEOUT 10
    )

    add_executable(
        redisdb-listener-tests
        tests/integration/tcp_listener_test.cpp
    )

    target_link_libraries(
        redisdb-listener-tests
        PRIVATE redisdb-core
    )

    redisdb_enable_warnings(redisdb-listener-tests)

    add_test(
        NAME integration.tcp_listener
        COMMAND redisdb-listener-tests
    )

    set_tests_properties(
        integration.tcp_listener
        PROPERTIES TIMEOUT 20
    )
endif()
