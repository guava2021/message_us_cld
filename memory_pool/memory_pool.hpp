// memory_pool.hpp — SPSC-optimized slab allocator for HFT
//
// Architecture:
//  SPSCFreeList     ring buffer of void* — no CAS, acquire/release only
//  MemoryPool       mmap-backed block storage + SPSCFreeList
//                   SPSC: alloc() on consumer thread, free() on producer thread
//  ThreadLocalCache per-thread LIFO stack; hot path = zero atomics
//                   cache misses batch-access MemoryPool
//  SlabPool         8 size classes [64..8192 B], routes by size
//  PoolStats        atomic counters: allocs, frees, failures
//
// Optional: define POOL_NUMA before including to enable numa_alloc_local()
// backing (requires libnuma).

#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

#include <sys/mman.h>
#include <unistd.h>

#ifdef POOL_NUMA
#include <numa.h>
#include <sched.h>
#endif

#ifdef __x86_64__
#include <immintrin.h>
#define POOL_PAUSE() _mm_pause()
#else
#define POOL_PAUSE() __asm__ volatile("yield" ::: "memory")
#endif

namespace pool {

// ── constants & helpers ───────────────────────────────────────────────────────

inline constexpr size_t kCacheLine = 64;

inline constexpr size_t align_up(size_t n, size_t a) noexcept {
    return (n + a - 1u) & ~(a - 1u);
}

inline constexpr uint64_t next_pow2(uint64_t n) noexcept {
    if (n <= 1u)
        return 1u;
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1u;
}

// ── PoolOptions ────────────────────────────────────────────────────────────────

struct PoolOptions {
    bool huge_pages  = false;  // MAP_HUGETLB — fewer TLB misses (requires 2 MB-aligned size)
    bool lock_memory = false;  // mlock() — prevent hot-path page faults
    bool numa_local  = false;  // numa_alloc_local() — requires POOL_NUMA + libnuma
};

// ── PoolStats — monitoring counters ───────────────────────────────────────────

struct PoolStats {
    std::atomic<uint64_t> allocs{0};
    std::atomic<uint64_t> frees{0};
    std::atomic<uint64_t> alloc_failures{0};

    uint64_t outstanding() const noexcept {
        return allocs.load(std::memory_order_relaxed) - frees.load(std::memory_order_relaxed);
    }
};

// ── SPSCFreeList ───────────────────────────────────────────────────────────────
//
// Lock-free ring buffer of void*.
//   push() — producer thread (free-er)
//   pop()  — consumer thread (alloc-er)
//
// Two atomics per op, both load-only on the fast path (no CAS).
// Capacity must be a power of two.

class SPSCFreeList {
   public:
    explicit SPSCFreeList(uint64_t capacity)
        : cap_(capacity), mask_(capacity - 1u), slots_(new void *[capacity]) {
        assert((capacity & (capacity - 1u)) == 0u && "capacity must be power of 2");
    }

    SPSCFreeList(const SPSCFreeList &)            = delete;
    SPSCFreeList &operator=(const SPSCFreeList &) = delete;

    // Producer: return block. Returns false only on overflow (signals a double-free bug).
    bool push(void *ptr) noexcept {
        const uint64_t w = wp_.load(std::memory_order_relaxed);
        if (w - rp_.load(std::memory_order_acquire) >= cap_)
            return false;
        slots_[w & mask_] = ptr;
        wp_.store(w + 1u, std::memory_order_release);
        return true;
    }

    // Consumer: take block. Returns nullptr if empty.
    void *pop() noexcept {
        const uint64_t r = rp_.load(std::memory_order_relaxed);
        if (r == wp_.load(std::memory_order_acquire))
            return nullptr;
        void *p = slots_[r & mask_];
        rp_.store(r + 1u, std::memory_order_release);
        return p;
    }

    uint64_t capacity() const noexcept { return cap_; }
    uint64_t size_approx() const noexcept {
        return wp_.load(std::memory_order_relaxed) - rp_.load(std::memory_order_relaxed);
    }

   private:
    // Separate cache lines to prevent false sharing between producer and consumer.
    alignas(kCacheLine) std::atomic<uint64_t> wp_{0};  // written by producer only
    alignas(kCacheLine) std::atomic<uint64_t> rp_{0};  // written by consumer only

    uint64_t cap_;
    uint64_t mask_;
    std::unique_ptr<void *[]> slots_;
};

// ── MemoryPool ────────────────────────────────────────────────────────────────
//
// Fixed-size block allocator over an mmap'd region.
//
// SPSC contract (caller responsibility):
//   alloc() — consumer thread only
//   free()  — producer thread only
//
// HFT pipeline: RX thread allocates message buffers; TX thread frees after send.
// Single-threaded use (same thread calls both) is also valid — SPSC degenerates
// to single-thread access with no races.

class MemoryPool {
   public:
    MemoryPool(size_t block_size, uint64_t num_blocks, PoolOptions opts = {})
        : block_size_(align_up(std::max(block_size, kCacheLine), kCacheLine)),
          num_blocks_(next_pow2(std::max(num_blocks, uint64_t{2}))),
          opts_(opts),
          freelist_(next_pow2(std::max(num_blocks, uint64_t{2}))) {
        map_backing();
        populate_freelist();
    }

    ~MemoryPool() noexcept {
        if (!backing_)
            return;
#ifdef POOL_NUMA
        if (numa_backing_)
            numa_free(backing_, backing_size_);
        else
#endif
            munmap(backing_, backing_size_);
    }

    MemoryPool(const MemoryPool &)            = delete;
    MemoryPool &operator=(const MemoryPool &) = delete;

    // Consumer thread: returns nullptr on pool exhaustion.
    [[nodiscard]] void *alloc() noexcept {
        void *p = freelist_.pop();
        if (__builtin_expect(p != nullptr, 1))
            stats_.allocs.fetch_add(1u, std::memory_order_relaxed);
        else
            stats_.alloc_failures.fetch_add(1u, std::memory_order_relaxed);
        return p;
    }

    // Producer thread: p must originate from this pool's alloc().
    void free(void *p) noexcept {
        assert(p && "free(nullptr)");
        assert(owns(p) && "pointer not owned by this pool");
        [[maybe_unused]] bool ok = freelist_.push(p);
        assert(ok && "freelist overflow — double-free or num_blocks mismatch");
        stats_.frees.fetch_add(1u, std::memory_order_relaxed);
    }

    size_t block_size() const noexcept { return block_size_; }
    uint64_t num_blocks() const noexcept { return num_blocks_; }
    uint64_t available() const noexcept { return freelist_.size_approx(); }
    const PoolStats &stats() const noexcept { return stats_; }

    // Debug / assert use only — not thread-safe relative to concurrent free().
    bool owns(const void *p) const noexcept {
        auto base = reinterpret_cast<uintptr_t>(backing_);
        auto addr = reinterpret_cast<uintptr_t>(p);
        return addr >= base && addr < base + backing_size_;
    }

   private:
    void map_backing() {
        backing_size_ = block_size_ * num_blocks_;

#ifdef POOL_NUMA
        if (opts_.numa_local && numa_available() >= 0) {
            backing_ = numa_alloc_local(backing_size_);
            if (!backing_)
                throw std::bad_alloc();
            numa_backing_ = true;
            if (opts_.lock_memory)
                mlock(backing_, backing_size_);
            return;
        }
#endif
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_HUGETLB
        if (opts_.huge_pages)
            flags |= MAP_HUGETLB;
#endif
        backing_ = mmap(nullptr, backing_size_, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (backing_ == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "mmap");
        if (opts_.lock_memory)
            mlock(backing_,
                  backing_size_);  // best-effort; silently fails if RLIMIT_MEMLOCK exceeded
    }

    void populate_freelist() {
        auto *base = static_cast<char *>(backing_);
        for (uint64_t i = 0; i < num_blocks_; ++i) {
            [[maybe_unused]] bool ok = freelist_.push(base + i * block_size_);
            assert(ok);
        }
    }

    size_t block_size_;
    uint64_t num_blocks_;
    PoolOptions opts_;
    SPSCFreeList freelist_;
    void *backing_                      = nullptr;
    size_t backing_size_                = 0;
    [[maybe_unused]] bool numa_backing_ = false;
    PoolStats stats_;
};

// ── ThreadLocalCache ──────────────────────────────────────────────────────────
//
// Per-thread LIFO stack of preallocated blocks.
//
// Hot path (stack hit):  zero atomics — pure array store/load.
// Cold path (miss/full): batch refill/flush to the underlying MemoryPool.
//
// Intended for threads that both allocate and free their own objects
// (single-threaded alloc+free loop).  For pipeline models where alloc and
// free are on different threads, call MemoryPool::alloc()/free() directly.
//
// LocalCapacity: per-thread stack depth.
// BatchSize:     blocks transferred per pool round-trip (≤ LocalCapacity/2).

template <size_t LocalCapacity = 64, size_t BatchSize = LocalCapacity / 2>
class ThreadLocalCache {
    static_assert(LocalCapacity >= 2 && BatchSize >= 1 && BatchSize <= LocalCapacity / 2,
                  "invalid cache parameters");

   public:
    explicit ThreadLocalCache(MemoryPool &pool) noexcept : pool_(pool), stack_{}, top_(0) {}

    ~ThreadLocalCache() { flush_all(); }

    ThreadLocalCache(const ThreadLocalCache &)            = delete;
    ThreadLocalCache &operator=(const ThreadLocalCache &) = delete;

    // Hot path: zero atomics. Returns nullptr only when pool is exhausted.
    [[nodiscard]] void *alloc() noexcept {
        if (__builtin_expect(top_ > 0, 1))
            return stack_[--top_];

        for (size_t i = 0; i < BatchSize; ++i) {
            void *p = pool_.alloc();
            if (!p)
                break;
            stack_[top_++] = p;
        }
        return top_ > 0 ? stack_[--top_] : nullptr;
    }

    // Hot path: zero atomics.
    void free(void *p) noexcept {
        if (__builtin_expect(top_ < LocalCapacity, 1)) {
            stack_[top_++] = p;
            return;
        }
        for (size_t i = 0; i < BatchSize; ++i)
            pool_.free(stack_[--top_]);
        stack_[top_++] = p;
    }

    // Return all cached blocks to pool. Call before thread exits or cache destruction.
    void flush_all() noexcept {
        while (top_ > 0)
            pool_.free(stack_[--top_]);
    }

    size_t cached_count() const noexcept { return top_; }

   private:
    MemoryPool &pool_;
    void *stack_[LocalCapacity];
    size_t top_ = 0;
};

// ── size class table ──────────────────────────────────────────────────────────

inline constexpr size_t kNumSizeClasses                  = 8;
inline constexpr size_t kSizeClassBytes[kNumSizeClasses] = {64,   128,  256,  512,
                                                            1024, 2048, 4096, 8192};

// Returns size-class index [0, 7], or -1 if n > 8192.
inline constexpr int size_to_class(size_t n) noexcept {
    if (n <= 64)
        return 0;
    if (n <= 128)
        return 1;
    if (n <= 256)
        return 2;
    if (n <= 512)
        return 3;
    if (n <= 1024)
        return 4;
    if (n <= 2048)
        return 5;
    if (n <= 4096)
        return 6;
    if (n <= 8192)
        return 7;
    return -1;
}

// ── SlabPool ──────────────────────────────────────────────────────────────────
//
// Multi-size-class allocator: one MemoryPool per size class.
// SPSC contract applies per pool.
// alloc(size) routes to the smallest class ≥ size.
// free(p, size) must be called with the same size as the original alloc(size).
// Sizes > 8192 bytes return nullptr — fall back to system allocator.

class SlabPool {
   public:
    explicit SlabPool(uint64_t blocks_per_class = 4096, PoolOptions opts = {}) {
        for (size_t i = 0; i < kNumSizeClasses; ++i)
            pools_[i] = std::make_unique<MemoryPool>(kSizeClassBytes[i], blocks_per_class, opts);
    }

    [[nodiscard]] void *alloc(size_t size) noexcept {
        const int cls = size_to_class(size);
        if (__builtin_expect(cls < 0, 0))
            return nullptr;
        return pools_[static_cast<size_t>(cls)]->alloc();
    }

    void free(void *p, size_t size) noexcept {
        const int cls = size_to_class(size);
        assert(cls >= 0 && "size > 8192 not managed by SlabPool");
        pools_[static_cast<size_t>(cls)]->free(p);
    }

    MemoryPool &pool(size_t class_idx) noexcept { return *pools_[class_idx]; }
    const MemoryPool &pool(size_t class_idx) const noexcept { return *pools_[class_idx]; }
    const PoolStats &stats(size_t class_idx) const noexcept { return pools_[class_idx]->stats(); }

   private:
    std::array<std::unique_ptr<MemoryPool>, kNumSizeClasses> pools_;
};

}  // namespace pool
