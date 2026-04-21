#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace hft {

// ── types ──────────────────────────────────────────────────────────────────
using TimerID  = uint64_t;
using Callback = std::function<void()>;

static constexpr TimerID kInvalidTimer = 0;

// ── EpollTimer ──────────────────────────────────────────────────────────────
//
// Single-threaded timer manager: one timerfd in one epoll instance, backed by
// a min-heap of pending entries.  Designed to live on a dedicated thread;
// run() / run_once() must NOT be called from multiple threads simultaneously.
//
// Precision: CLOCK_MONOTONIC, ~1 ms jitter (kernel timer resolution).
// For sub-100 µs accuracy use TimerWheel + TSC spin (timer_wheel.hpp).
//
class EpollTimer {
   public:
    struct Stats {
        uint64_t fired{0};
        uint64_t cancelled{0};
        uint64_t rearms{0};
        uint64_t overruns{0};  // timerfd overrun count (missed ticks)
    };

    explicit EpollTimer();
    ~EpollTimer() noexcept;

    // Non-copyable, non-movable (owns fds).
    EpollTimer(const EpollTimer &)            = delete;
    EpollTimer &operator=(const EpollTimer &) = delete;

    // Schedule a one-shot callback delay_ns nanoseconds from now.
    // Returns a TimerID that can be passed to cancel().
    [[nodiscard]] TimerID schedule(int64_t delay_ns, Callback cb);

    // Schedule a periodic callback; first fire is interval_ns from now.
    [[nodiscard]] TimerID schedule_periodic(int64_t interval_ns, Callback cb);

    // Cancel a pending timer.  Returns true if the timer was found and removed.
    bool cancel(TimerID id);

    // Block until the next timer fires (or timeout_ms elapses), then drain all
    // expired timers.  Pass timeout_ms = 0 for non-blocking poll.
    // Returns number of callbacks invoked.
    int run_once(int timeout_ms = -1);

    // Drive the event loop until stop() is called.
    void run();

    // Signal run() to return.  Safe to call from a signal handler.
    void stop() noexcept { running_.store(false, std::memory_order_relaxed); }

    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }

    // Return the number of pending (not-yet-fired) timers.
    [[nodiscard]] std::size_t pending() const noexcept { return active_.size(); }

   private:
    struct Entry {
        TimerID id{kInvalidTimer};
        int64_t expiry_ns{0};    // absolute CLOCK_MONOTONIC nanoseconds
        int64_t interval_ns{0};  // 0 = one-shot
        Callback cb;

        bool operator>(const Entry &o) const noexcept { return expiry_ns > o.expiry_ns; }
    };

    using MinHeap = std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>;

    int epoll_fd_{-1};
    int timer_fd_{-1};

    MinHeap heap_;
    // active_ is the source of truth for "is this ID still live?".
    // Cancelled IDs are removed here; stale heap entries are ignored on pop.
    std::unordered_map<TimerID, Entry> active_;

    std::atomic<bool> running_{false};
    std::atomic<TimerID> next_id_{1};
    Stats stats_;

    static int64_t now_ns() noexcept;
    void rearm_timerfd(int64_t expiry_ns);
    int drain_expired();
    void push_entry(Entry e);
};

}  // namespace hft
