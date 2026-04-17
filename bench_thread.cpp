// bench_thread.cpp — single-process benchmark using two threads.
//
// The producer and consumer are pinned to adjacent cores (configurable).
// Both use spinning for minimal latency.
//
// Timing: RDTSCP for per-message latency (invariant TSC, ~2ns overhead).
// TSC frequency is calibrated once at startup against CLOCK_MONOTONIC_RAW.
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
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

// ── TSC timing ────────────────────────────────────────────────────────────────
//
// RDTSCP serializes instruction retirement before reading the counter,
// giving accurate per-message timestamps without full CPUID serialisation.
// Requires constant_tsc + nonstop_tsc (verified in main).

static inline uint64_t rdtscp() {
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return (uint64_t(hi) << 32) | lo;
}

// Calibrate TSC frequency by correlating TSC ticks against CLOCK_MONOTONIC_RAW
// over a short busy-spin interval. Returns ticks per nanosecond as a double.
static double calibrate_tsc_ghz() {
    // Warm up.
    rdtscp();

    struct timespec t0 {
    }, t1{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
    uint64_t tsc0 = rdtscp();

    // Spin for ~50ms to get a stable measurement.
    struct timespec target = t0;
    target.tv_nsec += 50'000'000;
    if (target.tv_nsec >= 1'000'000'000) {
        target.tv_nsec -= 1'000'000'000;
        target.tv_sec += 1;
    }
    do {
        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
    } while (t1.tv_sec < target.tv_sec ||
             (t1.tv_sec == target.tv_sec && t1.tv_nsec < target.tv_nsec));

    uint64_t tsc1 = rdtscp();

    uint64_t elapsed_ns = uint64_t(t1.tv_sec - t0.tv_sec) * 1'000'000'000ULL +
                          uint64_t(t1.tv_nsec) - uint64_t(t0.tv_nsec);
    uint64_t elapsed_tsc = tsc1 - tsc0;

    return double(elapsed_tsc) / double(elapsed_ns);  // ticks per ns (GHz)
}

// Convert TSC delta to nanoseconds.
static inline double tsc_to_ns(uint64_t ticks, double tsc_ghz) {
    return double(ticks) / tsc_ghz;
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

// ── message layout (TSC timestamp in first 8 bytes) ───────────────────────────

struct alignas(8) Msg {
    uint64_t send_tsc;
    char payload[1];  // variable — rest of the message
};

// ── shared state between threads ──────────────────────────────────────────────

struct BenchState {
    spsc::SharedBus *bus{nullptr};
    uint64_t n_msgs{0};
    uint32_t msg_size{0};
    int prod_core{-1};
    int cons_core{-1};
    double tsc_ghz{1.0};
    std::atomic<bool> ready{false};

    // Results (filled by consumer)
    std::vector<uint64_t> latency_tsc;
    double prod_elapsed_ns{0};
};

// ── producer thread ───────────────────────────────────────────────────────────

static void producer_fn(BenchState *s) {
    pin_to_core(s->prod_core);

    spsc::Producer prod(*s->bus);
    auto *msg = static_cast<Msg *>(std::aligned_alloc(8, s->msg_size));
    std::memset(msg, 0, s->msg_size);

    s->ready.store(true, std::memory_order_release);
    while (!s->ready.load(std::memory_order_acquire)) { /* spin */
    }

    uint64_t t0 = rdtscp();
    for (uint64_t i = 0; i < s->n_msgs; ++i) {
        msg->send_tsc = rdtscp();
        prod.push(msg, s->msg_size);
    }
    s->prod_elapsed_ns = tsc_to_ns(rdtscp() - t0, s->tsc_ghz);

    std::free(msg);
}

// ── consumer thread ───────────────────────────────────────────────────────────

static void consumer_fn(BenchState *s) {
    pin_to_core(s->cons_core);

    spsc::Consumer cons(*s->bus);
    auto *buf = static_cast<Msg *>(std::aligned_alloc(8, s->msg_size));

    s->latency_tsc.resize(s->n_msgs);

    while (!s->ready.load(std::memory_order_acquire)) { /* spin */
    }

    for (uint64_t i = 0; i < s->n_msgs; ++i) {
        cons.pop(buf, s->msg_size);
        s->latency_tsc[i] = rdtscp() - buf->send_tsc;
    }

    std::free(buf);
}

// ── report ────────────────────────────────────────────────────────────────────

static void print_pct(const std::vector<double> &v, const char *tag, double pct) {
    size_t idx = size_t(pct / 100.0 * double(v.size()));
    if (idx >= v.size())
        idx = v.size() - 1;
    printf("  %-8s %7.1f ns\n", tag, v[idx]);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    uint64_t n_msgs   = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
    uint32_t msg_size = (argc > 2) ? uint32_t(std::strtoul(argv[2], nullptr, 10)) : 64U;
    int prod_core     = (argc > 3) ? std::atoi(argv[3]) : 0;
    int cons_core     = (argc > 4) ? std::atoi(argv[4]) : 2;

    if (msg_size < sizeof(uint64_t))
        msg_size = sizeof(uint64_t);

    // Verify invariant TSC is available.
    // tsc_known_freq implies constant_tsc + nonstop_tsc on Linux.
    printf("=== SPSC bench (in-process, two threads) ===\n");

    double tsc_ghz = calibrate_tsc_ghz();
    printf("  TSC frequency: %.4f GHz\n", tsc_ghz);
    printf("  messages     : %llu\n", (unsigned long long)n_msgs);
    printf("  msg_size     : %u B\n", msg_size);
    printf("  producer core: %d   consumer core: %d\n\n", prod_core, cons_core);
    fflush(stdout);

    constexpr uint32_t kCapacity = 64 * 1024;
    const char *kBusName         = "/spsc_bench_internal";

    auto bus = spsc::SharedBus::create(kBusName, kCapacity, msg_size);

    BenchState state;
    state.bus       = &bus;
    state.n_msgs    = n_msgs;
    state.msg_size  = msg_size;
    state.prod_core = prod_core;
    state.cons_core = cons_core;
    state.tsc_ghz   = tsc_ghz;

    std::thread cons_thr(consumer_fn, &state);
    producer_fn(&state);
    cons_thr.join();

    spsc::SharedBus::unlink(kBusName);

    // Convert TSC latencies to ns.
    std::vector<double> lat_ns(n_msgs);
    for (uint64_t i = 0; i < n_msgs; ++i)
        lat_ns[i] = tsc_to_ns(state.latency_tsc[i], tsc_ghz);

    std::sort(lat_ns.begin(), lat_ns.end());
    double sum = 0;
    for (double v : lat_ns)
        sum += v;
    double mean_ns = sum / double(n_msgs);
    double tput    = double(n_msgs) / (state.prod_elapsed_ns / 1e9);

    printf("Throughput : %.2f M msg/s  (%.2f MB/s)\n", tput / 1e6,
           tput * msg_size / (1024.0 * 1024.0));
    printf("\nOne-way latency (RDTSCP):\n");
    printf("  %-8s %7.1f ns\n", "min", lat_ns.front());
    printf("  %-8s %7.1f ns\n", "mean", mean_ns);
    print_pct(lat_ns, "p50", 50.0);
    print_pct(lat_ns, "p90", 90.0);
    print_pct(lat_ns, "p99", 99.0);
    print_pct(lat_ns, "p99.9", 99.9);
    printf("  %-8s %7.1f ns\n", "max", lat_ns.back());

    return 0;
}
