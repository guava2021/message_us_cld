# tcp_server — epoll-based TCP server with thread pool

add_library(tcp_server_lib
    src/protocol.cpp
    src/tcp_server.cpp
    src/tcp_client.cpp
)
target_include_directories(tcp_server_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tcp_server_lib PUBLIC common_headers pthread)

# Legacy skeleton target kept for compatibility
add_library(tcp_server INTERFACE)
target_include_directories(tcp_server INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcp_server INTERFACE common_headers)

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

    add_executable(tcp_unit_tests
        tests/unit/test_protocol.cpp
        tests/unit/test_thread_pool.cpp
    )
    target_link_libraries(tcp_unit_tests PRIVATE tcp_server_lib GTest::gtest_main pthread)

    add_executable(tcp_integration_tests
        tests/integration/test_loopback.cpp
    )
    target_link_libraries(tcp_integration_tests PRIVATE tcp_server_lib GTest::gtest_main pthread)

    add_executable(tcp_error_tests
        tests/integration/test_error_injection.cpp
    )
    target_link_libraries(tcp_error_tests PRIVATE tcp_server_lib GTest::gtest_main pthread)

    add_test(NAME TcpUnitTests COMMAND tcp_unit_tests)
    set_tests_properties(TcpUnitTests PROPERTIES TIMEOUT 60)
    add_test(NAME TcpIntegrationTests COMMAND tcp_integration_tests)
    set_tests_properties(TcpIntegrationTests PROPERTIES TIMEOUT 120)
    add_test(NAME TcpErrorTests COMMAND tcp_error_tests)
    set_tests_properties(TcpErrorTests PROPERTIES TIMEOUT 120)
endif()
