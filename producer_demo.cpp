// producer_demo.cpp — runs as a separate process; sends timestamped messages.
//
// Usage:  ./producer [bus_name] [n_messages] [msg_size]
//   e.g.  ./producer /spsc_demo 5000000 64

#include "spsc_bus.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

// Message layout: 8-byte send timestamp followed by payload padding.
struct alignas(8) MsgLayout {
    uint64_t send_ts_ns;
    char     payload[1];  // variable — rest of the message
};

int main(int argc, char** argv) {
    const char*  name      = (argc > 1) ? argv[1] : "/spsc_demo";
    uint64_t     n_msgs    = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 5'000'000;
    uint32_t     msg_size  = (argc > 3) ? uint32_t(std::strtoul(argv[3], nullptr, 10)) : 64u;

    if (msg_size < sizeof(uint64_t)) msg_size = sizeof(uint64_t);

    constexpr uint32_t kCapacity = 64 * 1024;  // 64K slots — ~4 MB for 64-byte msgs

    printf("[producer] bus='%s'  n=%llu  msg_size=%u  capacity=%u\n",
           name, (unsigned long long)n_msgs, msg_size, kCapacity);
    fflush(stdout);

    auto bus  = spsc::SharedBus::create(name, kCapacity, msg_size);
    spsc::Producer prod(bus);

    // Scratch buffer — stamp the send timestamp into the first 8 bytes.
    auto* msg = static_cast<MsgLayout*>(std::aligned_alloc(8, msg_size));
    if (!msg) { perror("aligned_alloc"); return 1; }
    std::memset(msg, 0xAB, msg_size);   // fill payload with pattern

    uint64_t t0 = now_ns();

    for (uint64_t i = 0; i < n_msgs; ++i) {
        msg->send_ts_ns = now_ns();
        prod.push(msg, msg_size);
    }

    uint64_t t1 = now_ns();
    double   elapsed_s = double(t1 - t0) / 1e9;
    double   throughput = double(n_msgs) / elapsed_s;
    double   bandwidth  = throughput * msg_size / (1024.0 * 1024.0);

    printf("[producer] sent %llu msgs in %.3f s  —  %.2f M msgs/s  %.2f MB/s\n",
           (unsigned long long)n_msgs, elapsed_s,
           throughput / 1e6, bandwidth);

    std::free(msg);

    // Leave the shm alive so the consumer can drain it; consumer will unlink.
    return 0;
}
