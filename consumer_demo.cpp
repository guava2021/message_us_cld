// consumer_demo.cpp — runs as a separate process; receives messages and reports
//                     one-way latency statistics with wall-clock timestamps.
//
// Usage:  ./consumer [bus_name] [n_messages] [msg_size]
//   e.g.  ./consumer /spsc_demo 5000000 64

#include "spsc_bus.hpp"
#include "tsc_clock.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

struct alignas(8) MsgLayout {
    uint64_t send_tsc;  // TSC stamp from producer — no syscall overhead
    char payload[1];
};

static void print_pct(const std::vector<double> &v, double pct) {
    if (v.empty())
        return;
    size_t idx = size_t(pct / 100.0 * double(v.size()));
    if (idx >= v.size())
        idx = v.size() - 1;
    printf("  p%-5.3g = %8.1f ns\n", pct, v[idx]);
}

int main(int argc, char **argv) {
    const char *name  = (argc > 1) ? argv[1] : "/spsc_demo";
    uint64_t n_msgs   = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 5'000'000;
    uint32_t msg_size = (argc > 3) ? uint32_t(std::strtoul(argv[3], nullptr, 10)) : 64U;

    if (msg_size < sizeof(uint64_t))
        msg_size = sizeof(uint64_t);

    // Calibrate TSC clock. Consumer and producer calibrate independently —
    // both share the same hardware oscillator so GHz values will match.
    tsc::TscClock clk;
    printf("[consumer] TSC %.4f GHz\n", clk.ghz());

    // Background recalibration — keeps TSC↔REALTIME mapping fresh.
    std::atomic<bool> stop{false};
    std::thread recal([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            clk.recalibrate();
        }
    });

    printf("[consumer] bus='%s'  n=%llu  msg_size=%u\n", name, (unsigned long long)n_msgs,
           msg_size);
    fflush(stdout);

    // Retry open — producer may not have created the segment yet.
    spsc::SharedBus bus = [&]() -> spsc::SharedBus {
        while (true) {
            try {
                return spsc::SharedBus::open(name);
            } catch (...) {
                struct timespec ts {};
                ts.tv_nsec = 1'000'000;  // 1 ms
                nanosleep(&ts, nullptr);
            }
        }
    }();

    spsc::Consumer cons(bus);

    auto *buf = static_cast<MsgLayout *>(std::aligned_alloc(8, msg_size));
    if (!buf) {
        perror("aligned_alloc");
        return 1;
    }

    std::vector<uint64_t> lat_tsc;
    lat_tsc.reserve(n_msgs);

    // ── critical path ─────────────────────────────────────────────────────────
    uint64_t t0 = tsc::TscClock::now_tsc();

    for (uint64_t i = 0; i < n_msgs; ++i) {
        cons.pop(buf, msg_size);
        uint64_t recv_tsc = tsc::TscClock::now_tsc();  // RDTSCP — no syscall
        lat_tsc.push_back(recv_tsc - buf->send_tsc);
    }

    uint64_t t1 = tsc::TscClock::now_tsc();
    // ── end critical path ──────────────────────────────────────────────────────

    std::free(buf);

    // Convert TSC latencies to ns for reporting (offline — not in critical path).
    std::vector<double> lat_ns(n_msgs);
    for (uint64_t i = 0; i < n_msgs; ++i)
        lat_ns[i] = clk.delta_ns(lat_tsc[i]);

    std::sort(lat_ns.begin(), lat_ns.end());
    double sum = 0;
    for (double v : lat_ns)
        sum += v;

    double elapsed_s  = clk.delta_ns(t1 - t0) / 1e9;
    double throughput = double(n_msgs) / elapsed_s;
    double mean_ns    = sum / double(n_msgs);

    // Wall-clock time of first and last message (for audit / regulatory logging).
    int64_t first_wall_ns = clk.to_wall_ns(t0);
    int64_t last_wall_ns  = clk.to_wall_ns(t1);

    printf("[consumer] received %llu msgs in %.3f s  —  %.2f M msg/s\n", (unsigned long long)n_msgs,
           elapsed_s, throughput / 1e6);
    printf("[consumer] wall start: %lld ns  end: %lld ns\n", (long long)first_wall_ns,
           (long long)last_wall_ns);
    printf("\nOne-way latency (RDTSCP):\n");
    printf("  min    = %8.1f ns\n", lat_ns.front());
    printf("  mean   = %8.1f ns\n", mean_ns);
    print_pct(lat_ns, 50.0);
    print_pct(lat_ns, 90.0);
    print_pct(lat_ns, 99.0);
    print_pct(lat_ns, 99.9);
    printf("  max    = %8.1f ns\n", lat_ns.back());

    spsc::SharedBus::unlink(name);
    printf("[consumer] unlinked '%s'\n", name);

    stop.store(true, std::memory_order_relaxed);
    recal.join();
    return 0;
}
