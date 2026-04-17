# tcp_server — non-blocking TCP server with epoll for HFT
#
# Status: skeleton — implementation pending.

add_library(tcp_server INTERFACE)
target_include_directories(tcp_server INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcp_server INTERFACE common_headers)
