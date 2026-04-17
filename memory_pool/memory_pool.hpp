// memory_pool.hpp — lock-free fixed-size slab allocator for HFT
//
// Design (planned):
//  • Fixed block size set at construction — no fragmentation, O(1) alloc/free
//  • Free-list implemented as a lock-free Treiber stack (CAS on head)
//  • Backing store: contiguous mmap'd region, optionally mlock'd
//  • Alignment: blocks aligned to cache-line boundary to prevent false sharing
//  • Thread safety: any number of producers/consumers may alloc/free concurrently
//
// Intended use: order objects, market-data structs, network buffers —
// anything that is allocated at message arrival and freed after processing.
//
// Status: skeleton — implementation pending.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pool {

// TODO: implement MemoryPool

}  // namespace pool
