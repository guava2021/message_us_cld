# memory_pool — SPSC-optimized slab allocator for HFT

add_library(memory_pool INTERFACE)
target_include_directories(memory_pool INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memory_pool INTERFACE common_headers)

# ── optional NUMA backing ─────────────────────────────────────────────────────
option(POOL_NUMA "Enable NUMA-local pool backing via libnuma" OFF)
if(POOL_NUMA)
    find_library(NUMA_LIB numa REQUIRED)
    message(STATUS "memory_pool: NUMA enabled (${NUMA_LIB})")
    target_compile_definitions(memory_pool INTERFACE POOL_NUMA)
    target_link_libraries(memory_pool INTERFACE ${NUMA_LIB})
endif()

# ── benchmark ─────────────────────────────────────────────────────────────────
add_executable(pool_bench bench_pool.cpp)
target_link_libraries(pool_bench PRIVATE memory_pool pthread)

# ── unit tests ────────────────────────────────────────────────────────────────
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

    add_executable(pool_tests tests/test_memory_pool.cpp)
    target_link_libraries(pool_tests PRIVATE memory_pool GTest::gtest_main pthread)

    if(SANITIZE)
        add_test(NAME MemoryPoolTests COMMAND pool_tests)
        set_tests_properties(MemoryPoolTests PROPERTIES TIMEOUT 120)
    else()
        include(GoogleTest)
        gtest_discover_tests(pool_tests DISCOVERY_MODE PRE_TEST)
    endif()
endif()

# ── convenience target ────────────────────────────────────────────────────────
add_custom_target(run_pool_bench
    COMMAND pool_bench
    DEPENDS pool_bench
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running memory pool latency benchmark"
)
