// bench_pool.cpp — latency benchmark: MemoryPool vs ThreadLocalCache vs malloc
//
// Build:  cmake -B build && cmake --build build --target pool_bench
// Run:    cmake --build build --target run_pool_bench
//         OR: ./build/pool_bench

#include "memory_pool.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef __x86_64__
#include <x86intrin.h>
static inline uint64_t rdtscp() noexcept {
    uint32_t aux;
    return __rdtscp(&aux);
}
#else
#include <time.h>
static inline uint64_t rdtscp() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}
#endif

static constexpr int kWarmup = 50'000;
static constexpr int kIter   = 1'000'000;

static uint64_t pct(std::vector<uint64_t> &v, double p) {
    size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(v.size()));
    if (idx >= v.size())
        idx = v.size() - 1;
    std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(idx), v.end());
    return v[idx];
}

static void print_result(const char *label, std::vector<uint64_t> &al, std::vector<uint64_t> &fr) {
    printf(
        "%-42s alloc p50=%4llu p99=%5llu p999=%6llu cycles  "
        "free p50=%4llu p99=%5llu p999=%6llu cycles\n",
        label, (unsigned long long)pct(al, 50), (unsigned long long)pct(al, 99),
        (unsigned long long)pct(al, 99.9), (unsigned long long)pct(fr, 50),
        (unsigned long long)pct(fr, 99), (unsigned long long)pct(fr, 99.9));
}

// ── MemoryPool (SPSC freelist — 2 atomics per op) ────────────────────────────

static void bench_pool(const char *label, size_t block_sz) {
    pool::MemoryPool mp(block_sz, 65536);

    for (int i = 0; i < kWarmup; ++i) {
        void *p = mp.alloc();
        if (p)
            mp.free(p);
    }

    std::vector<uint64_t> al(kIter), fr(kIter);
    for (int i = 0; i < kIter; ++i) {
        uint64_t t0 = rdtscp();
        void *p     = mp.alloc();
        uint64_t t1 = rdtscp();
        if (p)
            mp.free(p);
        uint64_t t2 = rdtscp();
        al[i]       = t1 - t0;
        fr[i]       = t2 - t1;
    }
    print_result(label, al, fr);
}

// ── ThreadLocalCache (zero atomics on fast path) ──────────────────────────────

static void bench_cache(const char *label, size_t block_sz) {
    pool::MemoryPool mp(block_sz, 65536);
    pool::ThreadLocalCache<64> cache(mp);

    for (int i = 0; i < kWarmup; ++i) {
        void *p = cache.alloc();
        if (p)
            cache.free(p);
    }

    std::vector<uint64_t> al(kIter), fr(kIter);
    for (int i = 0; i < kIter; ++i) {
        uint64_t t0 = rdtscp();
        void *p     = cache.alloc();
        uint64_t t1 = rdtscp();
        if (p)
            cache.free(p);
        uint64_t t2 = rdtscp();
        al[i]       = t1 - t0;
        fr[i]       = t2 - t1;
    }
    print_result(label, al, fr);
}

// ── malloc/free baseline ──────────────────────────────────────────────────────

static void bench_malloc(const char *label, size_t block_sz) {
    for (int i = 0; i < kWarmup; ++i) {
        void *p = malloc(block_sz);
        free(p);
    }

    std::vector<uint64_t> al(kIter), fr(kIter);
    for (int i = 0; i < kIter; ++i) {
        uint64_t t0 = rdtscp();
        void *p     = malloc(block_sz);
        uint64_t t1 = rdtscp();
        free(p);
        uint64_t t2 = rdtscp();
        al[i]       = t1 - t0;
        fr[i]       = t2 - t1;
    }
    print_result(label, al, fr);
}

// ── SlabPool ──────────────────────────────────────────────────────────────────

static void bench_slab(const char *label, size_t alloc_sz) {
    pool::SlabPool slab(65536);

    for (int i = 0; i < kWarmup; ++i) {
        void *p = slab.alloc(alloc_sz);
        if (p)
            slab.free(p, alloc_sz);
    }

    std::vector<uint64_t> al(kIter), fr(kIter);
    for (int i = 0; i < kIter; ++i) {
        uint64_t t0 = rdtscp();
        void *p     = slab.alloc(alloc_sz);
        uint64_t t1 = rdtscp();
        if (p)
            slab.free(p, alloc_sz);
        uint64_t t2 = rdtscp();
        al[i]       = t1 - t0;
        fr[i]       = t2 - t1;
    }
    print_result(label, al, fr);
}

int main() {
    printf("Memory pool alloc/free latency  (%d iters, %d warmup)\n\n", kIter, kWarmup);

    bench_pool("MemoryPool 64B  (SPSC, 2 atomics/op)", 64);
    bench_pool("MemoryPool 256B (SPSC, 2 atomics/op)", 256);
    printf("\n");
    bench_cache("ThreadLocalCache<64> 64B  (0 atomics hot)", 64);
    bench_cache("ThreadLocalCache<64> 256B (0 atomics hot)", 256);
    printf("\n");
    bench_slab("SlabPool 64B  (size-class dispatch)", 64);
    bench_slab("SlabPool 256B (size-class dispatch)", 256);
    printf("\n");
    bench_malloc("malloc/free 64B  (glibc baseline)", 64);
    bench_malloc("malloc/free 256B (glibc baseline)", 256);

    return 0;
}
