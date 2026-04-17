cmake_minimum_required(VERSION 3.16)
project(spsc_bus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always emit compile_commands.json for clang-tidy / IDEs.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── sanitizer option ──────────────────────────────────────────────────────────
# Usage:  cmake -DSANITIZE=asan | msan | tsan
set(SANITIZE "" CACHE STRING "Sanitizer to enable: asan | msan | tsan")

if(SANITIZE STREQUAL "asan")
    message(STATUS "Sanitizer: ASan + UBSan")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
    add_link_options(-fsanitize=address,undefined)
elseif(SANITIZE STREQUAL "msan")
    message(STATUS "Sanitizer: MSan (requires clang)")
    add_compile_options(-fsanitize=memory -fno-omit-frame-pointer
                        -fsanitize-memory-track-origins=2 -O1 -g)
    add_link_options(-fsanitize=memory)
elseif(SANITIZE STREQUAL "tsan")
    message(STATUS "Sanitizer: TSan")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -O1 -g)
    add_link_options(-fsanitize=thread)
else()
    # Production / benchmark builds — full optimisation.
    add_compile_options(-O3 -march=native)
endif()

# ── optional clang-tidy ───────────────────────────────────────────────────────
# Usage:  cmake -DENABLE_CLANG_TIDY=ON
option(ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)
if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    message(STATUS "clang-tidy: ${CLANG_TIDY_EXE}")
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()

# ── compiler warnings ─────────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic)

# ── header-only library ───────────────────────────────────────────────────────
add_library(spsc_bus INTERFACE)
target_include_directories(spsc_bus INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# ── common link libraries ─────────────────────────────────────────────────────
set(SPSC_LINK_LIBS rt pthread)

# ── targets ───────────────────────────────────────────────────────────────────
add_executable(producer producer_demo.cpp)
target_link_libraries(producer PRIVATE spsc_bus ${SPSC_LINK_LIBS})

add_executable(consumer consumer_demo.cpp)
target_link_libraries(consumer PRIVATE spsc_bus ${SPSC_LINK_LIBS})

add_executable(bench bench_thread.cpp)
target_link_libraries(bench PRIVATE spsc_bus ${SPSC_LINK_LIBS})

add_executable(bench_clock bench_clock.cpp)
target_link_libraries(bench_clock PRIVATE spsc_bus ${SPSC_LINK_LIBS})

# ── unit tests (Google Test) ──────────────────────────────────────────────────
include(CTest)
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
        # Prevent GTest from overriding our compiler/linker settings on Windows.
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(tests tests/test_spsc_bus.cpp)
    target_link_libraries(tests PRIVATE spsc_bus GTest::gtest_main ${SPSC_LINK_LIBS})

    if(SANITIZE)
        # gtest_discover_tests runs the binary via cmake execute_process which causes
        # ASan shadow-memory init to SEGFAULT in Docker containers (bind-mounted paths).
        # Use add_test directly for sanitizer builds — all tests still run, just as one
        # ctest entry. Non-sanitizer builds use full per-test discovery for granularity.
        add_test(NAME SpscBusTests COMMAND tests)
        set_tests_properties(SpscBusTests PROPERTIES TIMEOUT 120)
    else()
        include(GoogleTest)
        gtest_discover_tests(tests)
    endif()
endif()

# ── convenience targets ───────────────────────────────────────────────────────

# cmake --build build --target run_bench
add_custom_target(run_bench
    COMMAND bench 10000000 64 0 2
    DEPENDS bench
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running single-process two-thread benchmark"
)

# cmake --build build --target run_ipc
add_custom_target(run_ipc
    COMMAND ${CMAKE_COMMAND} -E echo "Starting consumer in background..."
    COMMAND sh -c "$<TARGET_FILE:consumer> /spsc_demo 5000000 64 &"
    COMMAND ${CMAKE_COMMAND} -E sleep 0.2
    COMMAND ${CMAKE_COMMAND} -E echo "Starting producer..."
    COMMAND $<TARGET_FILE:producer> /spsc_demo 5000000 64
    DEPENDS producer consumer
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running cross-process IPC demo"
)
