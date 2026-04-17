// producer_demo.cpp — runs as a separate process; sends TSC-timestamped messages.
//
// Usage:  ./producer [bus_name] [n_messages] [msg_size]
//   e.g.  ./producer /spsc_demo 5000000 64

#include "spsc_bus.hpp"
#include "tsc_clock.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// Message layout: 8-byte TSC send timestamp followed by payload.
struct alignas(8) MsgLayout {
    uint64_t send_tsc;
    char payload[1];  // variable — rest of the message
};

int main(int argc, char **argv) {
    const char *name  = (argc > 1) ? argv[1] : "/spsc_demo";
    uint64_t n_msgs   = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 5'000'000;
    uint32_t msg_size = (argc > 3) ? uint32_t(std::strtoul(argv[3], nullptr, 10)) : 64U;

    if (msg_size < sizeof(uint64_t))
        msg_size = sizeof(uint64_t);

    // Calibrate TSC clock once at startup (~50 ms).
    tsc::TscClock clk;
    printf("[producer] TSC %.4f GHz\n", clk.ghz());

    // Background recalibration: refresh TSC↔REALTIME correlation every 5 s to
    // absorb NTP drift. Never touches the critical path.
    std::atomic<bool> stop{false};
    std::thread recal([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            clk.recalibrate();
        }
    });

    constexpr uint32_t kCapacity = 64 * 1024;

    printf("[producer] bus='%s'  n=%llu  msg_size=%u  capacity=%u\n", name,
           (unsigned long long)n_msgs, msg_size, kCapacity);
    fflush(stdout);

    auto bus = spsc::SharedBus::create(name, kCapacity, msg_size);
    spsc::Producer prod(bus);

    auto *msg = static_cast<MsgLayout *>(std::aligned_alloc(8, msg_size));
    if (!msg) {
        perror("aligned_alloc");
        return 1;
    }
    std::memset(msg, 0xAB, msg_size);

    // ── critical path ─────────────────────────────────────────────────────────
    uint64_t t0 = tsc::TscClock::now_tsc();

    for (uint64_t i = 0; i < n_msgs; ++i) {
        msg->send_tsc = tsc::TscClock::now_tsc();  // RDTSCP — no syscall
        prod.push(msg, msg_size);
    }

    uint64_t t1 = tsc::TscClock::now_tsc();
    // ── end critical path ──────────────────────────────────────────────────────

    double elapsed_s  = clk.delta_ns(t1 - t0) / 1e9;
    double throughput = double(n_msgs) / elapsed_s;
    double bandwidth  = throughput * msg_size / (1024.0 * 1024.0);

    printf("[producer] sent %llu msgs in %.3f s  —  %.2f M msg/s  %.2f MB/s\n",
           (unsigned long long)n_msgs, elapsed_s, throughput / 1e6, bandwidth);

    std::free(msg);

    stop.store(true, std::memory_order_relaxed);
    recal.join();
    return 0;
}
