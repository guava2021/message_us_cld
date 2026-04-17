// bench_clock.cpp — microbenchmark: RDTSCP vs clock_gettime vDSO cost.
//
// Measures the per-call overhead of each timestamp method in isolation,
// pinned to a single core to eliminate migration jitter.
//
// Usage:  ./bench_clock [n_samples] [core]
//   e.g.  ./bench_clock 10000000 0

#include "tsc_clock.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <time.h>

static void pin_to_core(int core) {
    if (core < 0)
        return;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
}

static void print_stats(const char *label, std::vector<double> &v) {
    std::sort(v.begin(), v.end());
    double sum  = std::accumulate(v.begin(), v.end(), 0.0);
    double mean = sum / double(v.size());
    size_t p50  = size_t(0.50 * double(v.size()));
    size_t p99  = size_t(0.99 * double(v.size()));
    size_t p999 = size_t(0.999 * double(v.size()));
    printf("%-22s  min=%5.1f  mean=%5.1f  p50=%5.1f  p99=%5.1f  p99.9=%5.1f  max=%5.1f  ns\n",
           label, v.front(), mean, v[p50], v[p99], v[p999], v.back());
}

int main(int argc, char **argv) {
    uint64_t n = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
    int core   = (argc > 2) ? std::atoi(argv[2]) : 0;

    pin_to_core(core);

    tsc::TscClock clk;

    printf("=== Clock microbenchmark ===\n");
    printf("  TSC frequency : %.4f GHz\n", clk.ghz());
    printf("  samples       : %llu\n", (unsigned long long)n);
    printf("  core          : %d\n\n", core);

    std::vector<double> rdtscp_ns(n);
    std::vector<double> realtime_ns(n);
    std::vector<double> monotonic_ns(n);
    std::vector<double> monotonic_raw_ns(n);
    std::vector<double> tai_ns(n);

    // ── RDTSCP ────────────────────────────────────────────────────────────────
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t t0  = tsc::TscClock::now_tsc();
        uint64_t t1  = tsc::TscClock::now_tsc();
        rdtscp_ns[i] = clk.delta_ns(t1 - t0);
    }

    // ── CLOCK_REALTIME ────────────────────────────────────────────────────────
    for (uint64_t i = 0; i < n; ++i) {
        struct timespec ts {};
        uint64_t t0 = tsc::TscClock::now_tsc();
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t t1    = tsc::TscClock::now_tsc();
        realtime_ns[i] = clk.delta_ns(t1 - t0);
    }

    // ── CLOCK_MONOTONIC ───────────────────────────────────────────────────────
    for (uint64_t i = 0; i < n; ++i) {
        struct timespec ts {};
        uint64_t t0 = tsc::TscClock::now_tsc();
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t t1     = tsc::TscClock::now_tsc();
        monotonic_ns[i] = clk.delta_ns(t1 - t0);
    }

    // ── CLOCK_MONOTONIC_RAW ───────────────────────────────────────────────────
    for (uint64_t i = 0; i < n; ++i) {
        struct timespec ts {};
        uint64_t t0 = tsc::TscClock::now_tsc();
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        uint64_t t1         = tsc::TscClock::now_tsc();
        monotonic_raw_ns[i] = clk.delta_ns(t1 - t0);
    }

    // ── CLOCK_TAI ─────────────────────────────────────────────────────────────
    for (uint64_t i = 0; i < n; ++i) {
        struct timespec ts {};
        uint64_t t0 = tsc::TscClock::now_tsc();
        clock_gettime(CLOCK_TAI, &ts);
        uint64_t t1 = tsc::TscClock::now_tsc();
        tai_ns[i]   = clk.delta_ns(t1 - t0);
    }

    printf("Per-call cost (measured with surrounding RDTSCP — includes ~2x RDTSCP overhead):\n\n");
    print_stats("RDTSCP (2x)", rdtscp_ns);
    print_stats("CLOCK_REALTIME", realtime_ns);
    print_stats("CLOCK_MONOTONIC", monotonic_ns);
    print_stats("CLOCK_MONOTONIC_RAW", monotonic_raw_ns);
    print_stats("CLOCK_TAI", tai_ns);

    printf("\nNote: RDTSCP row = cost of two back-to-back RDTSCP calls (the measurement\n");
    printf("      overhead itself). Subtract half from each clock_gettime row to get\n");
    printf("      the net single-call cost.\n");

    return 0;
}
