// consumer_demo.cpp — runs as a separate process; receives messages and reports
//                     one-way latency statistics.
//
// Usage:  ./consumer [bus_name] [n_messages] [msg_size]
//   e.g.  ./consumer /spsc_demo 5000000 64

#include "spsc_bus.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

struct alignas(8) MsgLayout {
    uint64_t send_ts_ns;
    char payload[1];
};

static void print_percentile(std::vector<uint64_t> &v, double pct) {
    if (v.empty())
        return;
    size_t idx = size_t(pct / 100.0 * double(v.size()));
    if (idx >= v.size())
        idx = v.size() - 1;
    printf("  p%-5.2g = %6llu ns\n", pct, (unsigned long long)v[idx]);
}

int main(int argc, char **argv) {
    const char *name  = (argc > 1) ? argv[1] : "/spsc_demo";
    uint64_t n_msgs   = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 5'000'000;
    uint32_t msg_size = (argc > 3) ? uint32_t(std::strtoul(argv[3], nullptr, 10)) : 64u;

    if (msg_size < sizeof(uint64_t))
        msg_size = sizeof(uint64_t);

    printf("[consumer] bus='%s'  n=%llu  msg_size=%u\n", name, (unsigned long long)n_msgs,
           msg_size);
    fflush(stdout);

    // Retry open — producer may not have created the segment yet.
    spsc::SharedBus bus = [&]() -> spsc::SharedBus {
        while (true) {
            try {
                return spsc::SharedBus::open(name);
            } catch (...) {
                // shm not created yet — spin
                struct timespec ts {
                    .tv_sec = 0, .tv_nsec = 1'000'000
                };
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

    std::vector<uint64_t> latencies;
    latencies.reserve(n_msgs);

    uint64_t t0 = now_ns();

    for (uint64_t i = 0; i < n_msgs; ++i) {
        cons.pop(buf, msg_size);
        uint64_t recv_ts = now_ns();
        uint64_t lat     = recv_ts - buf->send_ts_ns;
        latencies.push_back(lat);
    }

    uint64_t t1 = now_ns();
    std::free(buf);

    // Stats
    std::sort(latencies.begin(), latencies.end());
    uint64_t sum = 0;
    for (uint64_t l : latencies)
        sum += l;

    double elapsed_s  = double(t1 - t0) / 1e9;
    double throughput = double(n_msgs) / elapsed_s;
    double mean_ns    = double(sum) / double(n_msgs);

    printf("[consumer] received %llu msgs in %.3f s  —  %.2f M msgs/s\n",
           (unsigned long long)n_msgs, elapsed_s, throughput / 1e6);
    printf("\nOne-way latency (ns):\n");
    printf("  min   = %6llu ns\n", (unsigned long long)latencies.front());
    printf("  mean  = %6.0f ns\n", mean_ns);
    print_percentile(latencies, 50.0);
    print_percentile(latencies, 90.0);
    print_percentile(latencies, 99.0);
    print_percentile(latencies, 99.9);
    printf("  max   = %6llu ns\n", (unsigned long long)latencies.back());

    // Clean up the shared memory segment.
    spsc::SharedBus::unlink(name);
    printf("[consumer] unlinked '%s'\n", name);
    return 0;
}
