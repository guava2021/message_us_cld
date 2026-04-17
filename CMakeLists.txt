cmake_minimum_required(VERSION 3.16)
project(hft_components LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All executables land in the build root for simple ./build/<name> invocation.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

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
option(ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)
if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    message(STATUS "clang-tidy: ${CLANG_TIDY_EXE}")
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()

# ── compiler warnings ─────────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic)

# ── shared headers (tsc_clock.hpp etc.) ──────────────────────────────────────
add_library(common_headers INTERFACE)
target_include_directories(common_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common)

# ── components ────────────────────────────────────────────────────────────────
include(CTest)
add_subdirectory(spsc_bus)
add_subdirectory(memory_pool)
add_subdirectory(tcp_server)
