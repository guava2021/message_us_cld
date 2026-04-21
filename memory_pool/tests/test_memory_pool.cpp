// tests/test_memory_pool.cpp — unit tests for memory_pool.hpp
//
// Build:  cmake -B build -DBUILD_TESTING=ON && cmake --build build
// Run:    ctest --test-dir build --output-on-failure -R MemoryPool
// ASan:   cmake -B build_asan -DBUILD_TESTING=ON -DSANITIZE=asan
//         cmake --build build_asan && ctest --test-dir build_asan

// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" const char *__asan_default_options() {
    return "detect_leaks=0";
}

#include "memory_pool.hpp"

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// ── SPSCFreeList ──────────────────────────────────────────────────────────────

TEST(SPSCFreeList, EmptyPopReturnsNull) {
    pool::SPSCFreeList fl(4);
    ASSERT_EQ(fl.pop(), nullptr);
}

TEST(SPSCFreeList, PushPopOrdering) {
    pool::SPSCFreeList fl(8);
    int a = 1, b = 2, c = 3;
    ASSERT_TRUE(fl.push(&a));
    ASSERT_TRUE(fl.push(&b));
    ASSERT_TRUE(fl.push(&c));
    ASSERT_EQ(fl.pop(), &a);
    ASSERT_EQ(fl.pop(), &b);
    ASSERT_EQ(fl.pop(), &c);
    ASSERT_EQ(fl.pop(), nullptr);
}

TEST(SPSCFreeList, FullReturnsFalse) {
    pool::SPSCFreeList fl(2);
    int x = 0, y = 0, z = 0;
    ASSERT_TRUE(fl.push(&x));
    ASSERT_TRUE(fl.push(&y));
    ASSERT_FALSE(fl.push(&z));
}

TEST(SPSCFreeList, SizeApprox) {
    pool::SPSCFreeList fl(4);
    ASSERT_EQ(fl.size_approx(), 0u);
    int x;
    ASSERT_TRUE(fl.push(&x));
    ASSERT_EQ(fl.size_approx(), 1u);
    fl.pop();
    ASSERT_EQ(fl.size_approx(), 0u);
}

TEST(SPSCFreeList, WrapAround) {
    pool::SPSCFreeList fl(4);
    int vals[8]{};
    // Fill and drain multiple times to verify wrap-around
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 4; ++i)
            ASSERT_TRUE(fl.push(&vals[i]));
        for (int i = 0; i < 4; ++i)
            ASSERT_NE(fl.pop(), nullptr);
    }
    ASSERT_EQ(fl.pop(), nullptr);
}

TEST(SPSCFreeList, ConcurrentSPSC) {
    constexpr int N = 200'000;
    pool::SPSCFreeList fl(1024);
    std::vector<int> data(N);

    std::atomic<int> received{0};

    std::thread producer([&] {
        for (int i = 0; i < N; ++i)
            while (!fl.push(&data[i]))
                POOL_PAUSE();
    });

    std::thread consumer([&] {
        for (int i = 0; i < N; ++i) {
            void *p;
            while (!(p = fl.pop()))
                POOL_PAUSE();
            (void)p;
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();
    ASSERT_EQ(received.load(), N);
}

// ── MemoryPool ────────────────────────────────────────────────────────────────

TEST(MemoryPool, BasicAllocFree) {
    pool::MemoryPool mp(64, 8);
    void *p = mp.alloc();
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(mp.owns(p));
    mp.free(p);
    ASSERT_EQ(mp.stats().allocs.load(), 1u);
    ASSERT_EQ(mp.stats().frees.load(), 1u);
    ASSERT_EQ(mp.stats().outstanding(), 0u);
}

TEST(MemoryPool, BlocksAreCacheLineAligned) {
    pool::MemoryPool mp(64, 16);
    for (int i = 0; i < 16; ++i) {
        void *p = mp.alloc();
        ASSERT_NE(p, nullptr);
        ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % pool::kCacheLine, 0u)
            << "block " << i << " is not cache-line aligned";
    }
}

TEST(MemoryPool, SmallBlockSizeRoundsUpToCacheLine) {
    pool::MemoryPool mp(1, 4);
    ASSERT_EQ(mp.block_size(), pool::kCacheLine);
}

TEST(MemoryPool, ExactSizeRoundedProperly) {
    pool::MemoryPool mp(65, 4);  // 65 → rounds up to 128
    ASSERT_EQ(mp.block_size(), 128u);
}

TEST(MemoryPool, AllBlocksUnique) {
    pool::MemoryPool mp(64, 8);
    std::set<void *> seen;
    for (int i = 0; i < 8; ++i) {
        void *p = mp.alloc();
        ASSERT_NE(p, nullptr);
        ASSERT_TRUE(seen.insert(p).second) << "duplicate block at index " << i;
    }
}

TEST(MemoryPool, ExhaustionReturnsNull) {
    pool::MemoryPool mp(64, 4);
    std::vector<void *> ptrs;
    for (int i = 0; i < 4; ++i)
        ptrs.push_back(mp.alloc());
    ASSERT_EQ(mp.alloc(), nullptr);
    ASSERT_EQ(mp.stats().alloc_failures.load(), 1u);
    for (void *p : ptrs)
        mp.free(p);
}

TEST(MemoryPool, BlockRecycledAfterFree) {
    pool::MemoryPool mp(64, 2);
    void *p1 = mp.alloc();
    void *p2 = mp.alloc();
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    mp.free(p1);
    void *p3 = mp.alloc();
    ASSERT_NE(p3, nullptr);  // pool has one slot free again
}

TEST(MemoryPool, NumBlocksRoundedToPow2) {
    pool::MemoryPool mp(64, 5);  // 5 → rounds up to 8
    ASSERT_EQ(mp.num_blocks(), 8u);
}

TEST(MemoryPool, OwnsBoundsCheck) {
    pool::MemoryPool mp(64, 4);
    void *p = mp.alloc();
    ASSERT_TRUE(mp.owns(p));
    int stack_var;
    ASSERT_FALSE(mp.owns(&stack_var));
    mp.free(p);
}

TEST(MemoryPool, WritableBlocks) {
    pool::MemoryPool mp(64, 4);
    void *p = mp.alloc();
    ASSERT_NE(p, nullptr);
    // Write and read back 64 bytes — verifies the mmap'd region is writable
    auto *bytes = static_cast<uint8_t *>(p);
    for (int i = 0; i < 64; ++i)
        bytes[i] = static_cast<uint8_t>(i);
    for (int i = 0; i < 64; ++i)
        ASSERT_EQ(bytes[i], static_cast<uint8_t>(i));
    mp.free(p);
}

TEST(MemoryPool, SPSCPipelineStress) {
    // Consumer thread allocates; producer thread frees.
    // Pool has only 256 slots — recycling is required.
    constexpr uint64_t N = 500'000;
    pool::MemoryPool mp(64, 256);

    std::vector<void *> channel(N, nullptr);
    std::atomic<uint64_t> produced{0};

    std::thread freer([&] {
        uint64_t freed = 0;
        while (freed < N) {
            uint64_t avail = produced.load(std::memory_order_acquire);
            while (freed < avail) {
                mp.free(channel[freed]);
                ++freed;
            }
            POOL_PAUSE();
        }
    });

    for (uint64_t i = 0; i < N; ++i) {
        void *p;
        while (!(p = mp.alloc()))
            POOL_PAUSE();
        channel[i] = p;
        produced.store(i + 1, std::memory_order_release);
    }
    freer.join();

    ASSERT_EQ(mp.stats().allocs.load(), N);
    ASSERT_EQ(mp.stats().frees.load(), N);
    ASSERT_EQ(mp.stats().outstanding(), 0u);
}

// ── ThreadLocalCache ──────────────────────────────────────────────────────────

TEST(ThreadLocalCache, BasicAllocFree) {
    pool::MemoryPool mp(64, 256);
    pool::ThreadLocalCache<32> cache(mp);  // LocalCapacity=32, BatchSize=16

    // First alloc: cache empty → refill BatchSize=16, return 1 → 15 remain
    void *p = cache.alloc();
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(cache.cached_count(), 15u);

    cache.free(p);
    ASSERT_EQ(cache.cached_count(), 16u);
}

TEST(ThreadLocalCache, HotPathCounterBehavior) {
    pool::MemoryPool mp(64, 256);
    pool::ThreadLocalCache<8, 4> cache(mp);  // LocalCapacity=8, BatchSize=4

    // First alloc triggers refill of 4 from pool, returns 1 → 3 remain
    void *p = cache.alloc();
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(cache.cached_count(), 3u);

    cache.free(p);
    ASSERT_EQ(cache.cached_count(), 4u);
}

TEST(ThreadLocalCache, FlushAll) {
    pool::MemoryPool mp(64, 256);
    {
        pool::ThreadLocalCache<16> cache(mp);
        for (int i = 0; i < 8; ++i) {
            void *p = cache.alloc();
            ASSERT_NE(p, nullptr);
            cache.free(p);  // back into cache
        }
        ASSERT_GT(cache.cached_count(), 0u);
        cache.flush_all();
        ASSERT_EQ(cache.cached_count(), 0u);
    }
    // Destructor calls flush_all(); all blocks returned to pool.
    ASSERT_EQ(mp.stats().outstanding(), 0u);
}

TEST(ThreadLocalCache, ExhaustsGracefully) {
    pool::MemoryPool mp(64, 8);
    pool::ThreadLocalCache<4, 2> cache(mp);

    std::vector<void *> ptrs;
    void *p;
    while ((p = cache.alloc()) != nullptr)
        ptrs.push_back(p);
    ASSERT_EQ(p, nullptr);  // pool exhausted

    for (void *q : ptrs)
        cache.free(q);
}

// ── size_to_class ─────────────────────────────────────────────────────────────

TEST(SizeClass, Boundaries) {
    ASSERT_EQ(pool::size_to_class(1), 0);
    ASSERT_EQ(pool::size_to_class(64), 0);
    ASSERT_EQ(pool::size_to_class(65), 1);
    ASSERT_EQ(pool::size_to_class(128), 1);
    ASSERT_EQ(pool::size_to_class(129), 2);
    ASSERT_EQ(pool::size_to_class(256), 2);
    ASSERT_EQ(pool::size_to_class(8192), 7);
    ASSERT_EQ(pool::size_to_class(8193), -1);
}

// ── SlabPool ──────────────────────────────────────────────────────────────────

TEST(SlabPool, BasicAllocFreeBySize) {
    pool::SlabPool slab(64);

    void *p64  = slab.alloc(64);
    void *p128 = slab.alloc(128);
    void *p512 = slab.alloc(512);
    ASSERT_NE(p64, nullptr);
    ASSERT_NE(p128, nullptr);
    ASSERT_NE(p512, nullptr);

    slab.free(p64, 64);
    slab.free(p128, 128);
    slab.free(p512, 512);

    ASSERT_EQ(slab.stats(0).outstanding(), 0u);  // class 0 = 64B
    ASSERT_EQ(slab.stats(1).outstanding(), 0u);  // class 1 = 128B
    ASSERT_EQ(slab.stats(3).outstanding(), 0u);  // class 3 = 512B
}

TEST(SlabPool, OversizeReturnsNull) {
    pool::SlabPool slab(64);
    ASSERT_EQ(slab.alloc(8193), nullptr);
    ASSERT_EQ(slab.alloc(1'000'000), nullptr);
}

TEST(SlabPool, SizeRoundsUpToClass) {
    pool::SlabPool slab(64);
    // alloc(100) → class 1 (128B blocks)
    void *p = slab.alloc(100);
    ASSERT_NE(p, nullptr);
    slab.free(p, 100);  // size_to_class(100) = 1 — same class as alloc
    ASSERT_EQ(slab.stats(1).outstanding(), 0u);
}

TEST(SlabPool, AllSizeClassesWork) {
    pool::SlabPool slab(16);
    for (size_t i = 0; i < pool::kNumSizeClasses; ++i) {
        size_t sz = pool::kSizeClassBytes[i];
        void *p   = slab.alloc(sz);
        ASSERT_NE(p, nullptr) << "class " << i << " (" << sz << "B) exhausted";
        slab.free(p, sz);
    }
}

TEST(SlabPool, ExhaustionPerClass) {
    pool::SlabPool slab(4);  // only 4 blocks per class (rounds to 4 = power of 2)
    std::vector<void *> ptrs;
    void *p;
    while ((p = slab.alloc(64)) != nullptr)
        ptrs.push_back(p);
    ASSERT_EQ(slab.alloc(64), nullptr);

    for (void *q : ptrs)
        slab.free(q, 64);
    ASSERT_NE(slab.alloc(64), nullptr);  // refilled
}
