cmake_minimum_required(VERSION 3.16)
project(spsc_bus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── compiler flags ────────────────────────────────────────────────────────────

add_compile_options(
    -O3
    -march=native
    -Wall
    -Wextra
    -Wpedantic
)

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
