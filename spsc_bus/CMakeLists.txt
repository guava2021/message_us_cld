# spsc_bus — lock-free SPSC message bus over POSIX shared memory

# Header-only interface library: includes spsc_bus.hpp + common/tsc_clock.hpp.
add_library(spsc_bus INTERFACE)
target_include_directories(spsc_bus INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spsc_bus INTERFACE common_headers)

set(SPSC_LINK_LIBS rt pthread)

add_executable(spsc_producer producer_demo.cpp)
target_link_libraries(spsc_producer PRIVATE spsc_bus ${SPSC_LINK_LIBS})

add_executable(spsc_consumer consumer_demo.cpp)
target_link_libraries(spsc_consumer PRIVATE spsc_bus ${SPSC_LINK_LIBS})

add_executable(spsc_bench bench_thread.cpp)
target_link_libraries(spsc_bench PRIVATE spsc_bus ${SPSC_LINK_LIBS})

add_executable(spsc_bench_clock bench_clock.cpp)
target_link_libraries(spsc_bench_clock PRIVATE spsc_bus ${SPSC_LINK_LIBS})

# ── unit tests (Google Test) ──────────────────────────────────────────────────
if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
            GIT_SHALLOW    TRUE
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(spsc_tests tests/test_spsc_bus.cpp)
    target_link_libraries(spsc_tests PRIVATE spsc_bus GTest::gtest_main ${SPSC_LINK_LIBS})

    if(SANITIZE)
        # gtest_discover_tests runs the binary via cmake execute_process which causes
        # ASan shadow-memory init to SEGFAULT in Docker containers (bind-mounted paths).
        # Use add_test directly for sanitizer builds — all tests still run, just as one
        # ctest entry. Non-sanitizer builds use full per-test discovery for granularity.
        add_test(NAME SpscBusTests COMMAND spsc_tests)
        set_tests_properties(SpscBusTests PROPERTIES TIMEOUT 120)
    else()
        include(GoogleTest)
        gtest_discover_tests(spsc_tests DISCOVERY_MODE PRE_TEST)
    endif()
endif()

# ── convenience targets ───────────────────────────────────────────────────────

# cmake --build build --target run_spsc_bench
add_custom_target(run_spsc_bench
    COMMAND spsc_bench 10000000 64 0 2
    DEPENDS spsc_bench
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running SPSC single-process benchmark"
)

# cmake --build build --target run_spsc_ipc
add_custom_target(run_spsc_ipc
    COMMAND ${CMAKE_COMMAND} -E echo "Starting consumer in background..."
    COMMAND sh -c "$<TARGET_FILE:spsc_consumer> /spsc_demo 5000000 64 &"
    COMMAND ${CMAKE_COMMAND} -E sleep 0.2
    COMMAND ${CMAKE_COMMAND} -E echo "Starting producer..."
    COMMAND $<TARGET_FILE:spsc_producer> /spsc_demo 5000000 64
    DEPENDS spsc_producer spsc_consumer
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running cross-process IPC demo"
)
