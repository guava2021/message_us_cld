# memory_pool — lock-free fixed-size slab allocator for HFT
#
# Status: skeleton — implementation pending.

add_library(memory_pool INTERFACE)
target_include_directories(memory_pool INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memory_pool INTERFACE common_headers)
