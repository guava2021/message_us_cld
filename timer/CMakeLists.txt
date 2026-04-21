cmake_minimum_required(VERSION 3.16)
project(hft_timer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── library ───────────────────────────────────────────────────────────────
add_library(hft_timer_lib
    src/epoll_timer.cpp
)
target_include_directories(hft_timer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(hft_timer_lib PRIVATE -Wall -Wextra -Wpedantic)

# ── tests ─────────────────────────────────────────────────────────────────
find_package(GTest QUIET)
if(GTest_FOUND OR TARGET GTest::gtest)
    enable_testing()

    add_executable(test_epoll_timer  tests/test_epoll_timer.cpp)
    target_link_libraries(test_epoll_timer PRIVATE hft_timer_lib GTest::gtest GTest::gtest_main)
    add_test(NAME epoll_timer COMMAND test_epoll_timer)

    add_executable(test_timer_wheel  tests/test_timer_wheel.cpp)
    target_link_libraries(test_timer_wheel PRIVATE hft_timer_lib GTest::gtest GTest::gtest_main)
    add_test(NAME timer_wheel COMMAND test_timer_wheel)
else()
    message(STATUS "GTest not found — timer tests skipped")
endif()
