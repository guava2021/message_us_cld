// tests/test_spsc_bus.cpp — unit tests for spsc_bus.hpp (Google Test)
//
// Build:  cmake -B build -DBUILD_TESTING=ON && cmake --build build
// Run:    ctest --test-dir build --output-on-failure
// ASan:   cmake -B build_asan -DBUILD_TESTING=ON -DSANITIZE=asan
//         cmake --build build_asan && ctest --test-dir build_asan

// ASan: disable LeakSanitizer — it requires /proc/self/maps which is
// unavailable inside Docker containers (used by both local CI and GitHub Actions).
// The name __asan_default_options is a well-known ASan hook and must use this
// exact reserved identifier; suppress the clang-tidy warning intentionally.
// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" const char *__asan_default_options() {
    return "detect_leaks=0";
}

#include "spsc_bus.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include <gtest/gtest.h>

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string unique_name() {
    static std::atomic<int> counter{0};
    return "/spsc_test_" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

// RAII: unlink shm segment on scope exit regardless of test outcome.
struct ShmGuard {
    std::string name;
    explicit ShmGuard(std::string n) : name(std::move(n)) {}
    ~ShmGuard() { spsc::SharedBus::unlink(name); }
    ShmGuard(const ShmGuard &)            = delete;
    ShmGuard &operator=(const ShmGuard &) = delete;
};

// ── basic correctness ─────────────────────────────────────────────────────────

TEST(SpscBus, BasicRoundTrip) {
    auto name = unique_name();
    ShmGuard g(name);
    auto bus = spsc::SharedBus::create(name, 16, 64);
    spsc::Producer prod(bus);
    spsc::Consumer cons(bus);

    const char payload[] = "hello_spsc";
    ASSERT_TRUE(prod.try_push(payload, sizeof(payload)));

    char buf[64]{};
    uint32_t n = cons.try_pop(buf, sizeof(buf));
    ASSERT_EQ(n, sizeof(payload));
    ASSERT_EQ(std::memcmp(buf, payload, n), 0);
}

TEST(SpscBus, MaxMsgSizeBoundaryAccepted) {
    auto name = unique_name();
    ShmGuard g(name);
    auto bus = spsc::SharedBus::create(name, 4, 8);
    spsc::Producer prod(bus);
    spsc::Consumer cons(bus);

    char msg[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(prod.try_push(msg, 8));

    char buf[8]{};
    ASSERT_EQ(cons.try_pop(buf, sizeof(buf)), 8u);
    ASSERT_EQ(std::memcmp(buf, msg, 8), 0);
}

TEST(SpscBus, OversizedMessageRejected) {
    auto name = unique_name();
    ShmGuard g(name);
    auto bus = spsc::SharedBus::create(name, 4, 8);
    spsc::Producer prod(bus);

    char msg[128]{};
    EXPECT_FALSE(prod.try_push(msg, 9));    // max + 1
    EXPECT_FALSE(prod.try_push(msg, 100));  // well over max

    // Ring must still be empty after rejections.
    spsc::Consumer cons(bus);
    EXPECT_EQ(cons.try_pop(msg, sizeof(msg)), 0u);
}

// ── ring boundary conditions ──────────────────────────────────────────────────

TEST(SpscBus, EmptyRingReturnsZero) {
    auto name = unique_name();
    ShmGuard g(name);
    auto bus = spsc::SharedBus::create(name, 4, 8);
    spsc::Consumer cons(bus);

    char buf[8]{};
    EXPECT_EQ(cons.try_pop(buf, sizeof(buf)), 0u);
}

TEST(SpscBus, FullRingReturnsFalse) {
    auto name = unique_name();
    ShmGuard g(name);
    constexpr uint32_t cap = 4;
    auto bus               = spsc::SharedBus::create(name, cap, 8);
    spsc::Producer prod(bus);

    char msg[8]{};
    for (uint32_t i = 0; i < cap; ++i)
        ASSERT_TRUE(prod.try_push(msg, 8));

    EXPECT_FALSE(prod.try_push(msg, 8));  // exactly at capacity
}

TEST(SpscBus, RingWrapsAcrossMultipleCycles) {
    auto name = unique_name();
    ShmGuard g(name);
    constexpr uint32_t cap = 4;
    auto bus               = spsc::SharedBus::create(name, cap, sizeof(uint32_t));
    spsc::Producer prod(bus);
    spsc::Consumer cons(bus);

    for (uint32_t cycle = 0; cycle < 3; ++cycle) {
        for (uint32_t i = 0; i < cap; ++i) {
            uint32_t val = cycle * cap + i;
            ASSERT_TRUE(prod.try_push(&val, sizeof(val)));
        }
        for (uint32_t i = 0; i < cap; ++i) {
            uint32_t val{};
            ASSERT_EQ(cons.try_pop(&val, sizeof(val)), sizeof(uint32_t));
            EXPECT_EQ(val, cycle * cap + i);
        }
    }
}

TEST(SpscBus, CapacityMinusOneAndCapacitySlotsInFlight) {
    auto name = unique_name();
    ShmGuard g(name);
    constexpr uint32_t cap = 8;
    auto bus               = spsc::SharedBus::create(name, cap, sizeof(uint32_t));
    spsc::Producer prod(bus);
    spsc::Consumer cons(bus);

    for (uint32_t fill : {cap - 1u, cap}) {
        for (uint32_t i = 0; i < fill; ++i)
            ASSERT_TRUE(prod.try_push(&i, sizeof(i)));
        for (uint32_t i = 0; i < fill; ++i) {
            uint32_t val{};
            ASSERT_EQ(cons.try_pop(&val, sizeof(val)), sizeof(uint32_t));
            EXPECT_EQ(val, i);
        }
    }
}

// ── validation ────────────────────────────────────────────────────────────────

TEST(SpscBusValidation, NonPowerOf2CapacityThrows) {
    for (uint32_t cap : {0u, 3u, 5u, 6u, 7u, 1023u}) {
        auto name = unique_name();
        EXPECT_THROW(spsc::SharedBus::create(name, cap, 8), std::invalid_argument)
            << "capacity=" << cap << " should throw";
        spsc::SharedBus::unlink(name);  // no-op if not created
    }
}

TEST(SpscBusValidation, PowerOf2CapacitiesAccepted) {
    for (uint32_t cap : {1u, 2u, 4u, 8u, 16u, 1024u}) {
        auto name = unique_name();
        ShmGuard g(name);
        EXPECT_NO_THROW(spsc::SharedBus::create(name, cap, 8))
            << "capacity=" << cap << " should be accepted";
    }
}

TEST(SpscBusValidation, ZeroMaxMsgSizeThrows) {
    auto name = unique_name();
    EXPECT_THROW(spsc::SharedBus::create(name, 4, 0), std::invalid_argument);
    spsc::SharedBus::unlink(name);
}

// ── concurrency ───────────────────────────────────────────────────────────────

TEST(SpscBusConcurrency, OrderingPreserved) {
    auto name = unique_name();
    ShmGuard g(name);
    constexpr uint64_t N = 100'000;

    auto bus = spsc::SharedBus::create(name, 1024, sizeof(uint64_t));
    spsc::Producer prod(bus);
    spsc::Consumer cons(bus);

    std::thread prod_thread([&]() {
        for (uint64_t i = 0; i < N; ++i)
            prod.push(&i, sizeof(i));
    });

    std::thread cons_thread([&]() {
        for (uint64_t i = 0; i < N; ++i) {
            uint64_t val{};
            cons.pop(&val, sizeof(val));
            EXPECT_EQ(val, i);
        }
    });

    prod_thread.join();
    cons_thread.join();
}

TEST(SpscBusConcurrency, ConsumerOpenRacesWithProducerInit) {
    // Tests the magic-spin path: consumer opens while producer is initialising.
    auto name = unique_name();
    ShmGuard g(name);
    constexpr uint32_t N   = 5000;
    constexpr uint32_t CAP = 1024;

    std::atomic<bool> bus_created{false};

    std::thread prod_thread([&]() {
        auto bus = spsc::SharedBus::create(name, CAP, sizeof(uint32_t));
        bus_created.store(true, std::memory_order_release);
        spsc::Producer prod(bus);
        for (uint32_t i = 0; i < N; ++i)
            prod.push(&i, sizeof(i));
    });

    std::thread cons_thread([&]() {
        while (!bus_created.load(std::memory_order_acquire)) {
        }
        auto bus = spsc::SharedBus::open(name);
        spsc::Consumer cons(bus);
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t val{};
            cons.pop(&val, sizeof(val));
            EXPECT_EQ(val, i);
        }
    });

    prod_thread.join();
    cons_thread.join();
}

// ── stress ────────────────────────────────────────────────────────────────────

TEST(SpscBusStress, OneMillion) {
    auto name = unique_name();
    ShmGuard g(name);
    constexpr uint64_t N = 1'000'000;

    auto bus = spsc::SharedBus::create(name, 4096, sizeof(uint64_t));
    spsc::Producer prod(bus);
    spsc::Consumer cons(bus);

    std::thread prod_thread([&]() {
        for (uint64_t i = 0; i < N; ++i)
            prod.push(&i, sizeof(i));
    });

    std::thread cons_thread([&]() {
        for (uint64_t i = 0; i < N; ++i) {
            uint64_t val{};
            cons.pop(&val, sizeof(val));
            EXPECT_EQ(val, i);
        }
    });

    prod_thread.join();
    cons_thread.join();
}
