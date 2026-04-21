#pragma GCC diagnostic ignored "-Wunused-result"
#include "epoll_timer.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

using namespace hft;
using namespace std::chrono_literals;

// ── helpers ────────────────────────────────────────────────────────────────

static int64_t ms_to_ns(int64_t ms) {
    return ms * 1'000'000LL;
}

// Drives run_once() until predicate is true or wall-clock deadline passes.
template <typename Pred>
static bool poll_until(EpollTimer &t, Pred pred, std::chrono::milliseconds budget) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        t.run_once(/*timeout_ms=*/5);
    }
    return true;
}

// ── one-shot ───────────────────────────────────────────────────────────────

TEST(EpollTimer, OneShotFires) {
    EpollTimer t;
    std::atomic<int> count{0};
    t.schedule(ms_to_ns(20), [&] { ++count; });
    EXPECT_TRUE(poll_until(t, [&] { return count.load() == 1; }, 200ms));
    EXPECT_EQ(t.stats().fired, 1u);
}

TEST(EpollTimer, OneShotFiresOnce) {
    EpollTimer t;
    std::atomic<int> count{0};
    t.schedule(ms_to_ns(20), [&] { ++count; });
    poll_until(t, [&] { return count.load() > 0; }, 200ms);
    // Keep pumping — should not fire again.
    for (int i = 0; i < 5; ++i)
        t.run_once(10);
    EXPECT_EQ(count.load(), 1);
}

TEST(EpollTimer, OneShotDelay) {
    EpollTimer t;
    auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point fired_at;
    bool fired = false;
    t.schedule(ms_to_ns(50), [&] {
        fired_at = std::chrono::steady_clock::now();
        fired    = true;
    });
    poll_until(t, [&] { return fired; }, 300ms);
    ASSERT_TRUE(fired);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(fired_at - start);
    EXPECT_GE(elapsed.count(), 40);   // at least 40 ms
    EXPECT_LT(elapsed.count(), 150);  // not more than 150 ms
}

// ── periodic ───────────────────────────────────────────────────────────────

TEST(EpollTimer, PeriodicFires) {
    EpollTimer t;
    std::atomic<int> count{0};
    t.schedule_periodic(ms_to_ns(20), [&] { ++count; });
    EXPECT_TRUE(poll_until(t, [&] { return count.load() >= 3; }, 500ms));
    EXPECT_GE(t.stats().fired, 3u);
}

TEST(EpollTimer, PeriodicStopsAfterCancel) {
    EpollTimer t;
    std::atomic<int> count{0};
    TimerID id = t.schedule_periodic(ms_to_ns(20), [&] { ++count; });
    poll_until(t, [&] { return count.load() >= 2; }, 300ms);
    t.cancel(id);
    int snapshot = count.load();
    // Pump a bit more — counter must not advance.
    for (int i = 0; i < 5; ++i)
        t.run_once(10);
    EXPECT_LE(count.load(), snapshot + 1);  // allow at most one more in-flight
}

// ── cancel ─────────────────────────────────────────────────────────────────

TEST(EpollTimer, CancelBeforeFire) {
    EpollTimer t;
    std::atomic<int> count{0};
    TimerID id = t.schedule(ms_to_ns(100), [&] { ++count; });
    bool ok    = t.cancel(id);
    EXPECT_TRUE(ok);
    // Pump well past the would-be deadline.
    for (int i = 0; i < 20; ++i)
        t.run_once(10);
    EXPECT_EQ(count.load(), 0);
    EXPECT_EQ(t.stats().cancelled, 1u);
    EXPECT_EQ(t.stats().fired, 0u);
}

TEST(EpollTimer, CancelInvalidIdReturnsFalse) {
    EpollTimer t;
    EXPECT_FALSE(t.cancel(9999));
}

TEST(EpollTimer, DoubleCancelReturnsFalse) {
    EpollTimer t;
    TimerID id = t.schedule(ms_to_ns(200), [] {});
    EXPECT_TRUE(t.cancel(id));
    EXPECT_FALSE(t.cancel(id));
}

// ── multiple timers ────────────────────────────────────────────────────────

TEST(EpollTimer, MultipleTimersOrdered) {
    EpollTimer t;
    std::vector<int> order;
    t.schedule(ms_to_ns(10), [&] { order.push_back(1); });
    t.schedule(ms_to_ns(30), [&] { order.push_back(2); });
    t.schedule(ms_to_ns(20), [&] { order.push_back(3); });
    poll_until(t, [&] { return order.size() >= 3; }, 300ms);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 3);
    EXPECT_EQ(order[2], 2);
}

TEST(EpollTimer, ManyTimers) {
    EpollTimer t;
    std::atomic<int> count{0};
    for (int i = 0; i < 100; ++i)
        t.schedule(ms_to_ns(10 + i % 20), [&] { ++count; });
    EXPECT_TRUE(poll_until(t, [&] { return count.load() == 100; }, 500ms));
    EXPECT_EQ(t.stats().fired, 100u);
}

// ── edge cases ─────────────────────────────────────────────────────────────

TEST(EpollTimer, ZeroDelayFiresImmediately) {
    EpollTimer t;
    std::atomic<bool> fired{false};
    t.schedule(0, [&] { fired = true; });
    EXPECT_TRUE(poll_until(t, [&] { return fired.load(); }, 200ms));
}

TEST(EpollTimer, RunOnceNonBlocking) {
    EpollTimer t;
    // No timers: run_once(0) must not block.
    auto start = std::chrono::steady_clock::now();
    t.run_once(0);
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 5ms);
}

TEST(EpollTimer, PendingCountAccurate) {
    EpollTimer t;
    EXPECT_EQ(t.pending(), 0u);
    TimerID a = t.schedule(ms_to_ns(500), [] {});
    TimerID b = t.schedule(ms_to_ns(500), [] {});
    EXPECT_EQ(t.pending(), 2u);
    t.cancel(a);
    EXPECT_EQ(t.pending(), 1u);
    t.cancel(b);
    EXPECT_EQ(t.pending(), 0u);
}

// ── background thread ──────────────────────────────────────────────────────

TEST(EpollTimer, RunInBackgroundThread) {
    EpollTimer t;
    std::atomic<int> count{0};
    t.schedule_periodic(ms_to_ns(10), [&] { ++count; });

    std::thread th([&] { t.run(); });

    // Wait for several fires then stop.
    std::this_thread::sleep_for(80ms);
    t.stop();
    th.join();

    EXPECT_GE(count.load(), 3);
}
