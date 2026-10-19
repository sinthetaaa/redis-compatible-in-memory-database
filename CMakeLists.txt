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
    src/protocol/RespValue.cpp
    src/net/UniqueFd.cpp
    src/net/TcpListener.cpp
    src/net/Connection.cpp
    src/net/ConnectionManager.cpp
)

if(APPLE)
    target_sources(
        redisdb-core
        PRIVATE src/net/KqueuePoller.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(
        redisdb-core
        PRIVATE src/net/EpollPoller.cpp
    )
endif()

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
    src/server/SmokeServer.cpp
)

target_link_libraries(
    redisdb-server
    PRIVATE redisdb-core
)

redisdb_enable_warnings(redisdb-server)

if(BUILD_TESTING)
    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)

        add_test(
            NAME integration.server_smoke
            COMMAND
                "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/integration/server_smoke_test.py"
                "$<TARGET_FILE:redisdb-server>"
        )

        set_tests_properties(
            integration.server_smoke
            PROPERTIES TIMEOUT 60
        )
    endif()

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

    add_executable(
        redisdb-resp-value-tests
        tests/unit/resp_value_test.cpp
    )

    target_link_libraries(
        redisdb-resp-value-tests
        PRIVATE redisdb-core
    )

    redisdb_enable_warnings(redisdb-resp-value-tests)

    add_test(
        NAME unit.resp_value
        COMMAND redisdb-resp-value-tests
    )

    set_tests_properties(
        unit.resp_value
        PROPERTIES TIMEOUT 10
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

    add_executable(
        redisdb-poller-tests
        tests/unit/event_poller_test.cpp
    )

    target_link_libraries(
        redisdb-poller-tests
        PRIVATE redisdb-core
    )

    redisdb_enable_warnings(redisdb-poller-tests)

    add_test(
        NAME unit.event_poller
        COMMAND redisdb-poller-tests
    )

    set_tests_properties(
        unit.event_poller
        PROPERTIES TIMEOUT 10
    )

    add_executable(
        redisdb-connection-tests
        tests/unit/connection_test.cpp
    )

    target_link_libraries(
        redisdb-connection-tests
        PRIVATE redisdb-core
    )

    redisdb_enable_warnings(redisdb-connection-tests)

    add_test(
        NAME unit.connection
        COMMAND redisdb-connection-tests
    )

    set_tests_properties(
        unit.connection
        PROPERTIES TIMEOUT 10
    )

    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(
            redisdb-manager-tests
            tests/integration/connection_manager_test.cpp
        )

        target_link_libraries(
            redisdb-manager-tests
            PRIVATE redisdb-core
        )

        redisdb_enable_warnings(redisdb-manager-tests)

        add_test(
            NAME integration.connection_manager
            COMMAND redisdb-manager-tests
        )

        set_tests_properties(
            integration.connection_manager
            PROPERTIES TIMEOUT 20
        )
    endif()

    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(
            redisdb-dispatch-tests
            tests/integration/connection_dispatch_test.cpp
        )

        target_link_libraries(
            redisdb-dispatch-tests
            PRIVATE redisdb-core
        )

        redisdb_enable_warnings(redisdb-dispatch-tests)

        add_test(
            NAME integration.connection_dispatch
            COMMAND redisdb-dispatch-tests
        )

        set_tests_properties(
            integration.connection_dispatch
            PROPERTIES TIMEOUT 20
        )
    endif()

    if(APPLE)
        add_executable(
            redisdb-kqueue-tests
            tests/integration/kqueue_poller_test.cpp
        )

        target_link_libraries(
            redisdb-kqueue-tests
            PRIVATE redisdb-core
        )

        redisdb_enable_warnings(redisdb-kqueue-tests)

        add_test(
            NAME integration.kqueue
            COMMAND redisdb-kqueue-tests
        )

        set_tests_properties(
            integration.kqueue
            PROPERTIES TIMEOUT 15
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Threads REQUIRED)

        add_executable(
            redisdb-epoll-tests
            tests/integration/epoll_poller_test.cpp
        )

        target_link_libraries(
            redisdb-epoll-tests
            PRIVATE
                redisdb-core
                Threads::Threads
        )

        redisdb_enable_warnings(redisdb-epoll-tests)

        add_test(
            NAME integration.epoll
            COMMAND redisdb-epoll-tests
        )

        set_tests_properties(
            integration.epoll
            PROPERTIES TIMEOUT 20
        )
    endif()
endif()
