// bench_thread.cpp — single-process benchmark using two threads.
//
// The producer and consumer are pinned to adjacent cores (configurable).
// Both use spinning for minimal latency.
//
// Usage:  ./bench [n_messages] [msg_size] [producer_core] [consumer_core]
//   e.g.  ./bench 10000000 64 0 2

#include "spsc_bus.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <numeric>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

// ── timing ───────────────────────────────────────────────────────────────────

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t(hi) << 32) | lo;
}

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

// ── cpu pinning ───────────────────────────────────────────────────────────────

static bool pin_to_core(int core) {
    if (core < 0)
        return true;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    return pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs) == 0;
}

// ── message layout (timestamp in first 8 bytes) ───────────────────────────────

struct alignas(8) Msg {
    uint64_t send_ts_ns;
    char payload[1];
};

// ── shared state between threads ──────────────────────────────────────────────

struct BenchState {
    spsc::SharedBus *bus;
    uint64_t n_msgs;
    uint32_t msg_size;
    int prod_core;
    int cons_core;
    std::atomic<bool> ready{false};

    // Results
    std::vector<uint64_t> latencies;
    double prod_elapsed_s{0};
};

// ── producer thread ───────────────────────────────────────────────────────────

static void producer_fn(BenchState *s) {
    pin_to_core(s->prod_core);

    spsc::Producer prod(*s->bus);
    auto *msg = static_cast<Msg *>(std::aligned_alloc(8, s->msg_size));
    std::memset(msg, 0, s->msg_size);

    // Signal ready and wait for consumer to be ready too.
    s->ready.store(true, std::memory_order_release);
    while (!s->ready.load(std::memory_order_acquire)) { /* spin */
    }

    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < s->n_msgs; ++i) {
        msg->send_ts_ns = now_ns();
        prod.push(msg, s->msg_size);
    }
    s->prod_elapsed_s = double(now_ns() - t0) / 1e9;

    std::free(msg);
}

// ── consumer thread ───────────────────────────────────────────────────────────

static void consumer_fn(BenchState *s) {
    pin_to_core(s->cons_core);

    spsc::Consumer cons(*s->bus);
    auto *buf = static_cast<Msg *>(std::aligned_alloc(8, s->msg_size));

    s->latencies.resize(s->n_msgs);

    // Wait for producer ready flag.
    while (!s->ready.load(std::memory_order_acquire)) { /* spin */
    }

    for (uint64_t i = 0; i < s->n_msgs; ++i) {
        cons.pop(buf, s->msg_size);
        uint64_t recv_ts = now_ns();
        s->latencies[i]  = recv_ts - buf->send_ts_ns;
    }

    std::free(buf);
}

// ── main ──────────────────────────────────────────────────────────────────────

static void print_pct(std::vector<uint64_t> &v, const char *tag, double pct) {
    size_t idx = size_t(pct / 100.0 * double(v.size()));
    if (idx >= v.size())
        idx = v.size() - 1;
    printf("  %-8s %6llu ns\n", tag, (unsigned long long)v[idx]);
}

int main(int argc, char **argv) {
    uint64_t n_msgs   = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
    uint32_t msg_size = (argc > 2) ? uint32_t(std::strtoul(argv[2], nullptr, 10)) : 64u;
    int prod_core     = (argc > 3) ? std::atoi(argv[3]) : 0;
    int cons_core     = (argc > 4) ? std::atoi(argv[4]) : 2;

    if (msg_size < sizeof(uint64_t))
        msg_size = sizeof(uint64_t);

    constexpr uint32_t kCapacity = 64 * 1024;
    const char *kBusName         = "/spsc_bench_internal";

    printf("=== SPSC bench (in-process, two threads) ===\n");
    printf("  messages   : %llu\n", (unsigned long long)n_msgs);
    printf("  msg_size   : %u B\n", msg_size);
    printf("  capacity   : %u slots\n", kCapacity);
    printf("  producer on core %d,  consumer on core %d\n\n", prod_core, cons_core);
    fflush(stdout);

    auto bus = spsc::SharedBus::create(kBusName, kCapacity, msg_size);

    BenchState state;
    state.bus       = &bus;
    state.n_msgs    = n_msgs;
    state.msg_size  = msg_size;
    state.prod_core = prod_core;
    state.cons_core = cons_core;

    std::thread cons_thr(consumer_fn, &state);
    producer_fn(&state);  // run producer on calling thread
    cons_thr.join();

    spsc::SharedBus::unlink(kBusName);

    // ── report ────────────────────────────────────────────────────────────────
    auto &lat = state.latencies;
    std::sort(lat.begin(), lat.end());
    double mean_ns = double(std::accumulate(lat.begin(), lat.end(), uint64_t(0))) / double(n_msgs);
    double tput    = double(n_msgs) / state.prod_elapsed_s;

    printf("Throughput : %.2f M msgs/s  (%.2f MB/s)\n", tput / 1e6,
           tput * msg_size / (1024.0 * 1024.0));
    printf("\nOne-way latency:\n");
    printf("  %-8s %6llu ns\n", "min", (unsigned long long)lat.front());
    printf("  %-8s %6.0f ns\n", "mean", mean_ns);
    print_pct(lat, "p50", 50.0);
    print_pct(lat, "p90", 90.0);
    print_pct(lat, "p99", 99.0);
    print_pct(lat, "p99.9", 99.9);
    printf("  %-8s %6llu ns\n", "max", (unsigned long long)lat.back());

    return 0;
}
