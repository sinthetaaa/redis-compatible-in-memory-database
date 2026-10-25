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
    src/commands/Command.cpp
    src/commands/CommandDispatcher.cpp
    src/commands/ArityValidator.cpp
    src/commands/ConnectivityCommands.cpp
    src/commands/StringCommands.cpp
    src/commands/KeyCommands.cpp
    src/commands/ListCommands.cpp
    src/commands/HashCommands.cpp
    src/commands/SetCommands.cpp
    src/commands/CommandErrors.cpp
    src/storage/Database.cpp
    src/storage/MemoryAccounting.cpp
    src/storage/Clock.cpp
    src/persistence/AofLog.cpp
    src/protocol/RespValue.cpp
    src/protocol/RespSerializer.cpp
    src/protocol/RespParser.cpp
    src/protocol/RespFramedInput.cpp
    src/protocol/RespPipeline.cpp
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
    src/server/RespCommandServer.cpp
)

target_link_libraries(
    redisdb-server
    PRIVATE redisdb-core
)

redisdb_enable_warnings(redisdb-server)

if(BUILD_TESTING)
    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)

        add_test(NAME integration.resp_memory_limits
                 COMMAND "${Python3_EXECUTABLE}"
                         "${PROJECT_SOURCE_DIR}/tests/integration/resp_memory_limits_test.py"
                         "$<TARGET_FILE:redisdb-server>")
        set_tests_properties(integration.resp_memory_limits PROPERTIES TIMEOUT 60)

        add_test(NAME integration.resp_aof
                 COMMAND "${Python3_EXECUTABLE}"
                         "${PROJECT_SOURCE_DIR}/tests/integration/resp_aof_test.py"
                         "$<TARGET_FILE:redisdb-server>")
        set_tests_properties(integration.resp_aof PROPERTIES TIMEOUT 90)

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

        add_test(
            NAME integration.resp_command_server
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_command_server_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_command_server PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_string_database
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_string_database_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_string_database PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_key_commands
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_key_commands_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_key_commands PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_expire_pexpire
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_expire_pexpire_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_expire_pexpire PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_ttl_pttl_persist
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_ttl_pttl_persist_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_ttl_pttl_persist PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_set_ex_px
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_set_ex_px_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_set_ex_px PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_active_expiration
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_active_expiration_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_active_expiration PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_expiration_acceptance
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_expiration_acceptance_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_expiration_acceptance PROPERTIES TIMEOUT 90)

        add_test(
            NAME integration.resp_list_commands
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_list_commands_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_list_commands PROPERTIES TIMEOUT 100)

        # Phase 6C.2: seven RESP2 hash commands tested over real TCP.
        add_test(
            NAME integration.resp_hash_commands
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_hash_commands_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_hash_commands PROPERTIES TIMEOUT 110)

        # Phase 6E.1A: real TCP cross-type WRONGTYPE acceptance matrix.
        add_test(
            NAME integration.resp_wrongtype_matrix
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_wrongtype_matrix_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_wrongtype_matrix PROPERTIES TIMEOUT 110)

        # Phase 6D.2: six RESP2 set commands tested on real multi-client TCP.
        add_test(
            NAME integration.resp_set_commands
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_set_commands_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_set_commands PROPERTIES TIMEOUT 110)

        add_test(
            NAME integration.resp_multi_string
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_multi_string_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_multi_string PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_append_strlen
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_append_strlen_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_append_strlen PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_integer_commands
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_integer_commands_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_integer_commands PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_numeric_edge_cases
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_numeric_edge_cases_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_numeric_edge_cases PROPERTIES TIMEOUT 75)

        add_test(
            NAME integration.resp_multi_client_correctness
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tests/integration/resp_multi_client_correctness_test.py"
                    "$<TARGET_FILE:redisdb-server>"
        )
        set_tests_properties(integration.resp_multi_client_correctness PROPERTIES TIMEOUT 110)
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
        redisdb-command-tests
        tests/unit/command_test.cpp
    )

    target_link_libraries(redisdb-command-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-command-tests)

    add_test(NAME unit.command COMMAND redisdb-command-tests)
    set_tests_properties(unit.command PROPERTIES TIMEOUT 10)

    add_executable(
        redisdb-command-dispatcher-tests
        tests/unit/command_dispatcher_test.cpp
    )

    target_link_libraries(redisdb-command-dispatcher-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-command-dispatcher-tests)

    add_test(NAME unit.command_dispatcher COMMAND redisdb-command-dispatcher-tests)
    set_tests_properties(unit.command_dispatcher PROPERTIES TIMEOUT 10)

    add_executable(
        redisdb-arity-validator-tests
        tests/unit/arity_validator_test.cpp
    )

    target_link_libraries(redisdb-arity-validator-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-arity-validator-tests)

    add_test(NAME unit.arity_validator COMMAND redisdb-arity-validator-tests)
    set_tests_properties(unit.arity_validator PROPERTIES TIMEOUT 10)

    add_executable(
        redisdb-connectivity-command-tests
        tests/unit/connectivity_commands_test.cpp
    )

    target_link_libraries(
        redisdb-connectivity-command-tests PRIVATE redisdb-core
    )
    redisdb_enable_warnings(redisdb-connectivity-command-tests)

    add_test(
        NAME unit.connectivity_commands
        COMMAND redisdb-connectivity-command-tests
    )
    set_tests_properties(
        unit.connectivity_commands PROPERTIES TIMEOUT 10
    )

    add_executable(
        redisdb-command-errors-tests
        tests/unit/command_errors_test.cpp
    )

    target_link_libraries(redisdb-command-errors-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-command-errors-tests)

    add_test(
        NAME unit.command_errors
        COMMAND redisdb-command-errors-tests
    )
    set_tests_properties(unit.command_errors PROPERTIES TIMEOUT 10)

    # Phase 6F.1B: pure deterministic memory-charge estimator (no Database ledger yet).
    add_executable(redisdb-memory-accounting-tests tests/unit/memory_accounting_test.cpp)
    target_link_libraries(redisdb-memory-accounting-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-memory-accounting-tests)
    add_test(NAME unit.memory_accounting COMMAND redisdb-memory-accounting-tests)
    set_tests_properties(unit.memory_accounting PROPERTIES TIMEOUT 15)

    # Phase 6A type-system groundwork: no collection command routes yet.
    add_executable(redisdb-storage-value-tests tests/unit/storage_value_test.cpp)
    target_link_libraries(redisdb-storage-value-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-storage-value-tests)
    add_test(NAME unit.storage_value COMMAND redisdb-storage-value-tests)
    set_tests_properties(unit.storage_value PROPERTIES TIMEOUT 15)

    # Phase 6B.2: six list command handlers; live RESP2 integration suite.
    add_executable(redisdb-list-command-tests tests/unit/list_commands_test.cpp)
    target_link_libraries(redisdb-list-command-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-list-command-tests)
    add_test(NAME unit.list_commands COMMAND redisdb-list-command-tests)
    set_tests_properties(unit.list_commands PROPERTIES TIMEOUT 20)

    # Phase 6B.1: owned list storage and safe cross-type command routing.
    add_executable(redisdb-list-foundation-tests tests/unit/list_storage_foundation_test.cpp)
    target_link_libraries(redisdb-list-foundation-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-list-foundation-tests)
    add_test(NAME unit.list_storage_foundation COMMAND redisdb-list-foundation-tests)
    set_tests_properties(unit.list_storage_foundation PROPERTIES TIMEOUT 20)

    # Phase 6C.1: hash storage foundation only; no RESP2 hash routes yet.
    add_executable(redisdb-hash-foundation-tests tests/unit/hash_storage_foundation_test.cpp)
    target_link_libraries(redisdb-hash-foundation-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-hash-foundation-tests)
    add_test(NAME unit.hash_storage_foundation COMMAND redisdb-hash-foundation-tests)
    set_tests_properties(unit.hash_storage_foundation PROPERTIES TIMEOUT 20)

    # Phase 6C.2: seven hash command handlers, type safety and response bounds.
    add_executable(redisdb-hash-command-tests tests/unit/hash_commands_test.cpp)
    target_link_libraries(redisdb-hash-command-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-hash-command-tests)
    add_test(NAME unit.hash_commands COMMAND redisdb-hash-command-tests)
    set_tests_properties(unit.hash_commands PROPERTIES TIMEOUT 20)

    # Phase 6F.2B: checked ledger transitions before database integration.
    add_executable(redisdb-ledger-transition-tests tests/unit/ledger_transitions_test.cpp)
    target_link_libraries(redisdb-ledger-transition-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-ledger-transition-tests)
    add_test(NAME unit.ledger_transitions COMMAND redisdb-ledger-transition-tests)
    set_tests_properties(unit.ledger_transitions PROPERTIES TIMEOUT 20)

    # Phase 6F.2C.1: live ledger across all Database mutation and expiry paths.
    add_executable(redisdb-live-ledger-tests tests/unit/live_memory_ledger_test.cpp)
    target_link_libraries(redisdb-live-ledger-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-live-ledger-tests)
    add_test(NAME unit.live_memory_ledger COMMAND redisdb-live-ledger-tests)
    set_tests_properties(unit.live_memory_ledger PROPERTIES TIMEOUT 30)

    add_executable(redisdb-memory-limits-tests tests/unit/memory_limits_test.cpp)
    target_link_libraries(redisdb-memory-limits-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-memory-limits-tests)
    add_test(NAME unit.memory_limits COMMAND redisdb-memory-limits-tests)
    set_tests_properties(unit.memory_limits PROPERTIES TIMEOUT 30)

    add_executable(redisdb-aof-tests tests/unit/aof_test.cpp)
    target_link_libraries(redisdb-aof-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-aof-tests)
    add_test(NAME unit.aof COMMAND redisdb-aof-tests)
    set_tests_properties(unit.aof PROPERTIES TIMEOUT 60)

    # Phase 6E.1A: cross-type WRONGTYPE matrix (four value families).
    add_executable(redisdb-wrongtype-matrix-tests tests/unit/wrongtype_matrix_test.cpp)
    target_link_libraries(redisdb-wrongtype-matrix-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-wrongtype-matrix-tests)
    add_test(NAME unit.wrongtype_matrix COMMAND redisdb-wrongtype-matrix-tests)
    set_tests_properties(unit.wrongtype_matrix PROPERTIES TIMEOUT 30)

    # Phase 6D.2: set RESP2 command handlers, type and response bounds.
    add_executable(redisdb-set-command-tests tests/unit/set_commands_test.cpp)
    target_link_libraries(redisdb-set-command-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-set-command-tests)
    add_test(NAME unit.set_commands COMMAND redisdb-set-command-tests)
    set_tests_properties(unit.set_commands PROPERTIES TIMEOUT 20)

    # Phase 6D.1: set-storage foundation (not yet network command routes).
    add_executable(redisdb-set-foundation-tests tests/unit/set_storage_foundation_test.cpp)
    target_link_libraries(redisdb-set-foundation-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-set-foundation-tests)
    add_test(NAME unit.set_storage_foundation COMMAND redisdb-set-foundation-tests)
    set_tests_properties(unit.set_storage_foundation PROPERTIES TIMEOUT 20)

    add_executable(
        redisdb-database-tests
        tests/unit/database_test.cpp
    )
    target_link_libraries(redisdb-database-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-database-tests)
    add_test(NAME unit.database COMMAND redisdb-database-tests)
    set_tests_properties(unit.database PROPERTIES TIMEOUT 20)

    add_executable(
        redisdb-clock-tests
        tests/unit/clock_test.cpp
    )
    target_link_libraries(redisdb-clock-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-clock-tests)
    add_test(NAME unit.clock COMMAND redisdb-clock-tests)
    set_tests_properties(unit.clock PROPERTIES TIMEOUT 15)

    add_executable(
        redisdb-expiration-metadata-tests
        tests/unit/expiration_metadata_test.cpp
    )
    target_link_libraries(redisdb-expiration-metadata-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-expiration-metadata-tests)
    add_test(NAME unit.expiration_metadata COMMAND redisdb-expiration-metadata-tests)
    set_tests_properties(unit.expiration_metadata PROPERTIES TIMEOUT 20)

    add_executable(redisdb-expire-command-tests tests/unit/expire_commands_test.cpp)
    target_link_libraries(redisdb-expire-command-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-expire-command-tests)
    add_test(NAME unit.expire_commands COMMAND redisdb-expire-command-tests)
    set_tests_properties(unit.expire_commands PROPERTIES TIMEOUT 20)

    add_executable(redisdb-ttl-command-tests tests/unit/ttl_pttl_persist_test.cpp)
    target_link_libraries(redisdb-ttl-command-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-ttl-command-tests)
    add_test(NAME unit.ttl_pttl_persist COMMAND redisdb-ttl-command-tests)
    set_tests_properties(unit.ttl_pttl_persist PROPERTIES TIMEOUT 20)

    add_executable(redisdb-set-ex-px-tests tests/unit/set_ex_px_test.cpp)
    target_link_libraries(redisdb-set-ex-px-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-set-ex-px-tests)
    add_test(NAME unit.set_ex_px COMMAND redisdb-set-ex-px-tests)
    set_tests_properties(unit.set_ex_px PROPERTIES TIMEOUT 20)

    add_executable(redisdb-active-expiration-tests tests/unit/active_expiration_test.cpp)
    target_link_libraries(redisdb-active-expiration-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-active-expiration-tests)
    add_test(NAME unit.active_expiration COMMAND redisdb-active-expiration-tests)
    set_tests_properties(unit.active_expiration PROPERTIES TIMEOUT 20)

    add_executable(redisdb-expiration-acceptance-tests tests/unit/expiration_acceptance_test.cpp)
    target_link_libraries(redisdb-expiration-acceptance-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-expiration-acceptance-tests)
    add_test(NAME unit.expiration_acceptance COMMAND redisdb-expiration-acceptance-tests)
    set_tests_properties(unit.expiration_acceptance PROPERTIES TIMEOUT 30)

    add_executable(redisdb-lazy-expiration-tests tests/unit/lazy_expiration_test.cpp)
    target_link_libraries(redisdb-lazy-expiration-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-lazy-expiration-tests)
    add_test(NAME unit.lazy_expiration COMMAND redisdb-lazy-expiration-tests)
    set_tests_properties(unit.lazy_expiration PROPERTIES TIMEOUT 20)

    add_executable(
        redisdb-string-command-tests
        tests/unit/string_commands_test.cpp
    )
    target_link_libraries(redisdb-string-command-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-string-command-tests)
    add_test(NAME unit.string_commands COMMAND redisdb-string-command-tests)
    set_tests_properties(unit.string_commands PROPERTIES TIMEOUT 15)

    add_executable(
        redisdb-multi-string-tests
        tests/unit/multi_string_commands_test.cpp
    )
    target_link_libraries(redisdb-multi-string-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-multi-string-tests)
    add_test(NAME unit.multi_string_commands COMMAND redisdb-multi-string-tests)
    set_tests_properties(unit.multi_string_commands PROPERTIES TIMEOUT 15)

    add_executable(
        redisdb-append-strlen-tests
        tests/unit/append_strlen_test.cpp
    )
    target_link_libraries(redisdb-append-strlen-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-append-strlen-tests)
    add_test(NAME unit.append_strlen COMMAND redisdb-append-strlen-tests)
    set_tests_properties(unit.append_strlen PROPERTIES TIMEOUT 15)

    add_executable(
        redisdb-integer-command-tests
        tests/unit/integer_commands_test.cpp
    )
    target_link_libraries(redisdb-integer-command-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-integer-command-tests)
    add_test(NAME unit.integer_commands COMMAND redisdb-integer-command-tests)
    set_tests_properties(unit.integer_commands PROPERTIES TIMEOUT 15)

    add_executable(
        redisdb-numeric-hardening-tests
        tests/unit/numeric_edge_cases_test.cpp
    )
    target_link_libraries(redisdb-numeric-hardening-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-numeric-hardening-tests)
    add_test(NAME unit.numeric_edge_cases COMMAND redisdb-numeric-hardening-tests)
    set_tests_properties(unit.numeric_edge_cases PROPERTIES TIMEOUT 20)

    add_executable(
        redisdb-multi-client-model-tests
        tests/unit/multi_client_model_test.cpp
    )
    target_link_libraries(redisdb-multi-client-model-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-multi-client-model-tests)
    add_test(NAME unit.multi_client_model COMMAND redisdb-multi-client-model-tests)
    set_tests_properties(unit.multi_client_model PROPERTIES TIMEOUT 20)

    add_executable(
        redisdb-key-command-tests
        tests/unit/key_commands_test.cpp
    )
    target_link_libraries(redisdb-key-command-tests PRIVATE redisdb-core)
    redisdb_enable_warnings(redisdb-key-command-tests)
    add_test(NAME unit.key_commands COMMAND redisdb-key-command-tests)
    set_tests_properties(unit.key_commands PROPERTIES TIMEOUT 15)

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

    add_executable(
        redisdb-resp-serializer-tests
        tests/unit/resp_serializer_test.cpp
    )

    target_link_libraries(
        redisdb-resp-serializer-tests
        PRIVATE redisdb-core
    )

    redisdb_enable_warnings(redisdb-resp-serializer-tests)

    add_test(
        NAME unit.resp_serializer
        COMMAND redisdb-resp-serializer-tests
    )

    set_tests_properties(
        unit.resp_serializer
        PROPERTIES TIMEOUT 10
    )

    add_executable(
        redisdb-resp-parser-tests
        tests/unit/resp_parser_test.cpp
    )

    target_link_libraries(
        redisdb-resp-parser-tests
        PRIVATE redisdb-core
    )

    redisdb_enable_warnings(redisdb-resp-parser-tests)

    add_test(
        NAME unit.resp_parser
        COMMAND redisdb-resp-parser-tests
    )

    set_tests_properties(
        unit.resp_parser
        PROPERTIES TIMEOUT 10
    )

    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(
            redisdb-resp-framed-input-tests
            tests/integration/resp_framed_input_test.cpp
        )

        target_link_libraries(
            redisdb-resp-framed-input-tests
            PRIVATE redisdb-core
        )

        redisdb_enable_warnings(
            redisdb-resp-framed-input-tests
        )

        add_test(
            NAME integration.resp_framed_input
            COMMAND redisdb-resp-framed-input-tests
        )

        set_tests_properties(
            integration.resp_framed_input
            PROPERTIES TIMEOUT 20
        )

        add_executable(
            redisdb-resp-pipeline-tests
            tests/integration/resp_pipeline_test.cpp
        )

        target_link_libraries(
            redisdb-resp-pipeline-tests
            PRIVATE redisdb-core
        )

        redisdb_enable_warnings(
            redisdb-resp-pipeline-tests
        )

        add_test(
            NAME integration.resp_pipeline
            COMMAND redisdb-resp-pipeline-tests
        )

        set_tests_properties(
            integration.resp_pipeline
            PROPERTIES TIMEOUT 20
        )

        add_executable(
            redisdb-resp-pipeline-dispatch-tests
            tests/integration/resp_pipeline_dispatch_test.cpp
        )

        target_link_libraries(
            redisdb-resp-pipeline-dispatch-tests
            PRIVATE redisdb-core
        )

        redisdb_enable_warnings(
            redisdb-resp-pipeline-dispatch-tests
        )

        add_test(
            NAME integration.resp_pipeline_dispatch
            COMMAND redisdb-resp-pipeline-dispatch-tests
        )

        set_tests_properties(
            integration.resp_pipeline_dispatch
            PROPERTIES TIMEOUT 30
        )

        add_executable(
            redisdb-resp-pipeline-scheduler-hardening-tests
            tests/integration/resp_pipeline_scheduler_hardening_test.cpp
        )

        target_link_libraries(
            redisdb-resp-pipeline-scheduler-hardening-tests
            PRIVATE redisdb-core
        )

        redisdb_enable_warnings(
            redisdb-resp-pipeline-scheduler-hardening-tests
        )

        add_test(
            NAME integration.resp_pipeline_scheduler_hardening
            COMMAND redisdb-resp-pipeline-scheduler-hardening-tests
        )

        set_tests_properties(
            integration.resp_pipeline_scheduler_hardening
            PROPERTIES TIMEOUT 30
        )

        add_executable(
            redisdb-resp-fragmented-dispatch-tests
            tests/integration/resp_fragmented_dispatch_test.cpp
        )

        target_link_libraries(
            redisdb-resp-fragmented-dispatch-tests
            PRIVATE redisdb-core
        )

        redisdb_enable_warnings(
            redisdb-resp-fragmented-dispatch-tests
        )

        add_test(
            NAME integration.resp_fragmented_dispatch
            COMMAND redisdb-resp-fragmented-dispatch-tests
        )

        set_tests_properties(
            integration.resp_fragmented_dispatch
            PROPERTIES TIMEOUT 20
        )
    endif()

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

# Phase 2G: opt-in isolated fuzz/sanitizer targets. Normal builds are unchanged.
option(REDISDB_ENABLE_FUZZ_TARGETS "Build standalone fuzz targets" OFF)
if(REDISDB_ENABLE_FUZZ_TARGETS)
    include(cmake/FuzzTargets.cmake)
endif()
