#include "epoll_timer.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace hft {

// ── helpers ────────────────────────────────────────────────────────────────

static void throw_errno(const char *what) {
    throw std::system_error(errno, std::generic_category(), what);
}

static struct itimerspec to_itimerspec(int64_t expiry_ns) noexcept {
    struct itimerspec ts {};
    ts.it_value.tv_sec  = expiry_ns / 1'000'000'000LL;
    ts.it_value.tv_nsec = expiry_ns % 1'000'000'000LL;
    // One-shot: no repeat interval (it_interval stays zero).
    return ts;
}

// ── EpollTimer ──────────────────────────────────────────────────────────────

EpollTimer::EpollTimer() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        throw_errno("epoll_create1");

    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd_ < 0) {
        ::close(epoll_fd_);
        throw_errno("timerfd_create");
    }

    struct epoll_event ev {};
    ev.events  = EPOLLIN;
    ev.data.fd = timer_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev) < 0) {
        ::close(timer_fd_);
        ::close(epoll_fd_);
        throw_errno("epoll_ctl(ADD)");
    }
}

EpollTimer::~EpollTimer() noexcept {
    if (timer_fd_ >= 0)
        ::close(timer_fd_);
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

// ── internal ───────────────────────────────────────────────────────────────

int64_t EpollTimer::now_ns() noexcept {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
}

void EpollTimer::rearm_timerfd(int64_t expiry_ns) {
    struct itimerspec ts = to_itimerspec(expiry_ns);
    // TFD_TIMER_ABSTIME: expiry_ns is absolute CLOCK_MONOTONIC.
    if (::timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &ts, nullptr) < 0)
        throw_errno("timerfd_settime");
    ++stats_.rearms;
}

void EpollTimer::push_entry(Entry e) {
    active_[e.id] = e;
    heap_.push(std::move(e));
    // Rearm if this entry is now the earliest deadline.
    if (heap_.top().id == active_.begin()->first ||
        heap_.top().expiry_ns == active_[heap_.top().id].expiry_ns) {
        rearm_timerfd(heap_.top().expiry_ns);
    }
}

int EpollTimer::drain_expired() {
    int count   = 0;
    int64_t now = now_ns();

    while (!heap_.empty()) {
        const Entry &top = heap_.top();

        // Skip stale heap entries (cancelled or already rescheduled).
        auto it = active_.find(top.id);
        if (it == active_.end() || it->second.expiry_ns != top.expiry_ns) {
            heap_.pop();
            continue;
        }

        if (top.expiry_ns > now)
            break;  // Not yet expired.

        Entry e = top;
        heap_.pop();
        active_.erase(e.id);

        e.cb();
        ++stats_.fired;
        ++count;

        if (e.interval_ns > 0) {
            // Reschedule periodic timer aligned to original cadence.
            e.expiry_ns += e.interval_ns;
            // If we're multiple intervals behind, jump ahead.
            while (e.expiry_ns <= now)
                e.expiry_ns += e.interval_ns;
            TimerID id  = e.id;
            active_[id] = e;
            heap_.push(std::move(e));
        }
    }

    // Rearm for the next earliest pending timer.
    while (!heap_.empty()) {
        const Entry &top = heap_.top();
        auto it          = active_.find(top.id);
        if (it == active_.end() || it->second.expiry_ns != top.expiry_ns) {
            heap_.pop();
            continue;
        }
        rearm_timerfd(top.expiry_ns);
        break;
    }

    return count;
}

// ── public API ──────────────────────────────────────────────────────────────

TimerID EpollTimer::schedule(int64_t delay_ns, Callback cb) {
    Entry e;
    e.id          = next_id_.fetch_add(1, std::memory_order_relaxed);
    e.expiry_ns   = now_ns() + delay_ns;
    e.interval_ns = 0;
    e.cb          = std::move(cb);

    bool need_rearm = heap_.empty() || e.expiry_ns < heap_.top().expiry_ns;
    active_[e.id]   = e;
    heap_.push(e);
    if (need_rearm)
        rearm_timerfd(e.expiry_ns);
    return e.id;
}

TimerID EpollTimer::schedule_periodic(int64_t interval_ns, Callback cb) {
    Entry e;
    e.id          = next_id_.fetch_add(1, std::memory_order_relaxed);
    e.expiry_ns   = now_ns() + interval_ns;
    e.interval_ns = interval_ns;
    e.cb          = std::move(cb);

    bool need_rearm = heap_.empty() || e.expiry_ns < heap_.top().expiry_ns;
    active_[e.id]   = e;
    heap_.push(e);
    if (need_rearm)
        rearm_timerfd(e.expiry_ns);
    return e.id;
}

bool EpollTimer::cancel(TimerID id) {
    auto it = active_.find(id);
    if (it == active_.end())
        return false;
    active_.erase(it);
    ++stats_.cancelled;
    // Stale heap entry will be discarded lazily in drain_expired().
    return true;
}

int EpollTimer::run_once(int timeout_ms) {
    struct epoll_event ev {};
    int n = ::epoll_wait(epoll_fd_, &ev, 1, timeout_ms);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        throw_errno("epoll_wait");
    }
    if (n == 0)
        return 0;  // Timeout — no event.

    // Read (and discard) the timerfd counter; record overruns.
    uint64_t overruns = 0;
    ssize_t rc        = ::read(timer_fd_, &overruns, sizeof(overruns));
    if (rc == sizeof(overruns) && overruns > 1)
        stats_.overruns += overruns - 1;

    return drain_expired();
}

void EpollTimer::run() {
    running_.store(true, std::memory_order_relaxed);
    while (running_.load(std::memory_order_relaxed)) {
        run_once(/*timeout_ms=*/10);
    }
}

}  // namespace hft
