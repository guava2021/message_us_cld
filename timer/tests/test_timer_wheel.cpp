#pragma GCC diagnostic ignored "-Wunused-result"
#include "timer_wheel.hpp"

#include <gtest/gtest.h>

using namespace hft;

// ── one-shot ───────────────────────────────────────────────────────────────

TEST(TimerWheel, OneShotFiresAtCorrectTick) {
    TimerWheel w;
    int count = 0;
    w.schedule(5, [&] { ++count; });
    for (int i = 0; i < 4; ++i) {
        w.tick();
        EXPECT_EQ(count, 0);
    }
    w.tick();  // tick 5
    EXPECT_EQ(count, 1);
    w.tick();             // tick 6
    EXPECT_EQ(count, 1);  // no repeat
}

TEST(TimerWheel, OneShotZeroDelayFiresNextTick) {
    TimerWheel w;
    int count = 0;
    w.schedule(0, [&] { ++count; });  // clamped to 1
    w.tick();
    EXPECT_EQ(count, 1);
}

// ── periodic ───────────────────────────────────────────────────────────────

TEST(TimerWheel, PeriodicFiresEveryInterval) {
    TimerWheel w;
    int count = 0;
    w.schedule_periodic(10, [&] { ++count; });
    for (int tick = 1; tick <= 50; ++tick) {
        w.tick();
        EXPECT_EQ(count, tick / 10);
    }
}

TEST(TimerWheel, PeriodicStopsAfterCancel) {
    TimerWheel w;
    int count  = 0;
    TimerID id = w.schedule_periodic(5, [&] { ++count; });
    for (int i = 0; i < 5; ++i)
        w.tick();
    EXPECT_EQ(count, 1);
    w.cancel(id);
    for (int i = 0; i < 20; ++i)
        w.tick();
    EXPECT_EQ(count, 1);
}

// ── cancel ─────────────────────────────────────────────────────────────────

TEST(TimerWheel, CancelBeforeFire) {
    TimerWheel w;
    int count  = 0;
    TimerID id = w.schedule(10, [&] { ++count; });
    w.cancel(id);
    for (int i = 0; i < 15; ++i)
        w.tick();
    EXPECT_EQ(count, 0);
    EXPECT_EQ(w.stats().cancelled, 1u);
}

TEST(TimerWheel, CancelInvalidReturnsFalse) {
    TimerWheel w;
    EXPECT_FALSE(w.cancel(9999));
}

TEST(TimerWheel, DoubleCancelReturnsFalse) {
    TimerWheel w;
    TimerID id = w.schedule(100, [] {});
    EXPECT_TRUE(w.cancel(id));
    EXPECT_FALSE(w.cancel(id));
}

// ── multiple timers ────────────────────────────────────────────────────────

TEST(TimerWheel, MultipleTimersOrdered) {
    TimerWheel w;
    std::vector<int> order;
    w.schedule(3, [&] { order.push_back(3); });
    w.schedule(1, [&] { order.push_back(1); });
    w.schedule(2, [&] { order.push_back(2); });
    w.advance(3);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(TimerWheel, SimultaneousTimersAllFire) {
    TimerWheel w;
    int count = 0;
    for (int i = 0; i < 10; ++i)
        w.schedule(5, [&] { ++count; });
    w.advance(5);
    EXPECT_EQ(count, 10);
}

TEST(TimerWheel, ManyTimers) {
    TimerWheel w;
    int count = 0;
    for (int i = 1; i <= 200; ++i)
        w.schedule(static_cast<uint64_t>(i), [&] { ++count; });
    w.advance(200);
    EXPECT_EQ(count, 200);
    EXPECT_EQ(w.stats().fired, 200u);
}

// ── cascading (level boundaries) ───────────────────────────────────────────

TEST(TimerWheel, CrossesL0L1Boundary) {
    TimerWheel w;
    int count = 0;
    // 256 ticks = exactly the L0→L1 boundary.
    w.schedule(256, [&] { ++count; });
    w.advance(256);
    EXPECT_EQ(count, 1);
}

TEST(TimerWheel, FarFutureCascade) {
    TimerWheel w;
    int count = 0;
    // Land in L2 (> 256*64 = 16384 ticks).
    w.schedule(20000, [&] { ++count; });
    w.advance(20000);
    EXPECT_EQ(count, 1);
}

TEST(TimerWheel, VeryFarFutureCascade) {
    TimerWheel w;
    int count = 0;
    // Land in L3 (> 256*64*64 = 1M ticks).
    constexpr uint64_t far = 1'100'000;
    w.schedule(far, [&] { ++count; });
    w.advance(far);
    EXPECT_EQ(count, 1);
}

// ── advance helper ─────────────────────────────────────────────────────────

TEST(TimerWheel, AdvanceMatchesRepeatedTick) {
    TimerWheel w1, w2;
    int c1 = 0, c2 = 0;
    w1.schedule(50, [&] { ++c1; });
    w2.schedule(50, [&] { ++c2; });
    w1.advance(100);
    for (int i = 0; i < 100; ++i)
        w2.tick();
    EXPECT_EQ(c1, c2);
}

// ── pending count ──────────────────────────────────────────────────────────

TEST(TimerWheel, PendingCountAccurate) {
    TimerWheel w;
    EXPECT_EQ(w.pending(), 0u);
    TimerID a = w.schedule(10, [] {});
    TimerID b = w.schedule(20, [] {});
    EXPECT_EQ(w.pending(), 2u);
    w.cancel(a);
    EXPECT_EQ(w.pending(), 1u);
    w.advance(20);
    EXPECT_EQ(w.pending(), 0u);
    (void)b;
}

// ── re-entrant scheduling ──────────────────────────────────────────────────

TEST(TimerWheel, CallbackReschedulesTimer) {
    TimerWheel w;
    int count = 0;
    // A one-shot that re-arms itself from within the callback.
    std::function<void()> cb;
    cb = [&] {
        ++count;
        if (count < 3)
            w.schedule(5, cb);
    };
    w.schedule(5, cb);
    w.advance(15);
    EXPECT_EQ(count, 3);
}
