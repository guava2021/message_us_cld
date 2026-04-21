#pragma once

// Hierarchical Timer Wheel — O(1) insert / cancel / tick
//
// Layout (default):
//   Level 0:  256 slots ×    1 ms =  256 ms  (fine wheel)
//   Level 1:   64 slots ×  256 ms =  ~16 s   (coarse wheel)
//   Level 2:   64 slots ×  ~16 s  =  ~17 min
//   Level 3:   64 slots ×  ~17min =  ~18 h
//
// Caller drives the wheel by calling tick() at a regular interval (typically
// every 1 ms from an epoll/timerfd loop, or every TSC quantum).
//
// All operations are O(1) except for cascading during tick(), which is O(k)
// where k is the number of timers cascading from a higher level — amortised
// O(1) per timer over its lifetime.
//
// NOT thread-safe.  Intended for a single timer thread.

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>

namespace hft {

using TimerID  = uint64_t;
using Callback = std::function<void()>;

static constexpr TimerID kWheelInvalidTimer = 0;

// ── slot counts per level ──────────────────────────────────────────────────
static constexpr std::size_t kL0Slots = 256;  // 2^8
static constexpr std::size_t kL1Slots = 64;   // 2^6
static constexpr std::size_t kL2Slots = 64;
static constexpr std::size_t kL3Slots = 64;

class TimerWheel {
   public:
    struct Stats {
        uint64_t inserted{0};
        uint64_t fired{0};
        uint64_t cancelled{0};
        uint64_t cascades{0};
    };

    // tick_duration_ns: granularity of one tick (default 1 ms).
    explicit TimerWheel(int64_t tick_duration_ns = 1'000'000LL);

    // ── API ────────────────────────────────────────────────────────────────

    // Insert a one-shot timer that fires after delay_ticks ticks.
    // Returns TimerID (never kWheelInvalidTimer on success).
    [[nodiscard]] TimerID schedule(uint64_t delay_ticks, Callback cb);

    // Insert a periodic timer; first fire after interval_ticks.
    [[nodiscard]] TimerID schedule_periodic(uint64_t interval_ticks, Callback cb);

    // Cancel a timer; returns true if it was still pending.
    bool cancel(TimerID id);

    // Advance the wheel by one tick.  Fires all timers that have expired.
    // Returns the number of callbacks invoked.
    int tick();

    // Advance by n ticks (equivalent to calling tick() n times but faster).
    int advance(uint64_t n);

    [[nodiscard]] uint64_t current_tick() const noexcept { return jiffies_; }
    [[nodiscard]] int64_t tick_ns() const noexcept { return tick_ns_; }
    [[nodiscard]] std::size_t pending() const noexcept { return entries_.size(); }
    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }

   private:
    struct Entry {
        TimerID id{kWheelInvalidTimer};
        uint64_t expiry_tick{0};
        uint64_t interval_ticks{0};  // 0 = one-shot
        Callback cb;
    };

    using Slot     = std::list<Entry>;
    using SlotIter = Slot::iterator;

    // Wheel levels stored in flat arrays.
    std::array<Slot, kL0Slots> l0_;
    std::array<Slot, kL1Slots> l1_;
    std::array<Slot, kL2Slots> l2_;
    std::array<Slot, kL3Slots> l3_;

    // Map TimerID → (level, slot_index, list iterator) for O(1) cancel.
    struct Location {
        int level{0};
        std::size_t slot{0};
        SlotIter iter;
    };
    std::unordered_map<TimerID, Location> entries_;

    uint64_t jiffies_{0};
    int64_t tick_ns_;
    uint64_t next_id_{1};
    Stats stats_;

    void insert(Entry e);
    int fire_slot(Slot &slot);
    int cascade(int from_level);
};

// ── inline implementation ──────────────────────────────────────────────────

inline TimerWheel::TimerWheel(int64_t tick_duration_ns) : tick_ns_(tick_duration_ns) {}

inline TimerID TimerWheel::schedule(uint64_t delay_ticks, Callback cb) {
    if (delay_ticks == 0)
        delay_ticks = 1;  // earliest possible: next tick
    Entry e;
    e.id             = next_id_++;
    e.expiry_tick    = jiffies_ + delay_ticks;
    e.interval_ticks = 0;
    e.cb             = std::move(cb);
    insert(e);
    ++stats_.inserted;
    return e.id;
}

inline TimerID TimerWheel::schedule_periodic(uint64_t interval_ticks, Callback cb) {
    if (interval_ticks == 0)
        interval_ticks = 1;
    Entry e;
    e.id             = next_id_++;
    e.expiry_tick    = jiffies_ + interval_ticks;
    e.interval_ticks = interval_ticks;
    e.cb             = std::move(cb);
    insert(e);
    ++stats_.inserted;
    return e.id;
}

inline bool TimerWheel::cancel(TimerID id) {
    auto it = entries_.find(id);
    if (it == entries_.end())
        return false;
    Location &loc = it->second;
    switch (loc.level) {
        case 0:
            l0_[loc.slot].erase(loc.iter);
            break;
        case 1:
            l1_[loc.slot].erase(loc.iter);
            break;
        case 2:
            l2_[loc.slot].erase(loc.iter);
            break;
        case 3:
            l3_[loc.slot].erase(loc.iter);
            break;
        default:
            break;
    }
    entries_.erase(it);
    ++stats_.cancelled;
    return true;
}

inline void TimerWheel::insert(Entry e) {
    uint64_t remaining = e.expiry_tick - jiffies_;
    int level{0};
    std::size_t slot{0};

    if (remaining < kL0Slots) {
        level = 0;
        slot  = e.expiry_tick & (kL0Slots - 1);
        l0_[slot].push_back(std::move(e));
        entries_[l0_[slot].back().id] = {level, slot, std::prev(l0_[slot].end())};
    } else if (remaining < kL0Slots * kL1Slots) {
        level = 1;
        slot  = (e.expiry_tick >> 8) & (kL1Slots - 1);
        l1_[slot].push_back(std::move(e));
        entries_[l1_[slot].back().id] = {level, slot, std::prev(l1_[slot].end())};
    } else if (remaining < kL0Slots * kL1Slots * kL2Slots) {
        level = 2;
        slot  = (e.expiry_tick >> 14) & (kL2Slots - 1);
        l2_[slot].push_back(std::move(e));
        entries_[l2_[slot].back().id] = {level, slot, std::prev(l2_[slot].end())};
    } else {
        level = 3;
        slot  = (e.expiry_tick >> 20) & (kL3Slots - 1);
        l3_[slot].push_back(std::move(e));
        entries_[l3_[slot].back().id] = {level, slot, std::prev(l3_[slot].end())};
    }
}

inline int TimerWheel::fire_slot(Slot &slot) {
    int count = 0;
    // Move entire slot out so callbacks may re-schedule without invalidating
    // our iteration (avoids re-entrancy issues).
    Slot local;
    local.swap(slot);
    for (auto &e : local) {
        entries_.erase(e.id);
        e.cb();
        ++stats_.fired;
        ++count;
        if (e.interval_ticks != 0) {
            // Reschedule periodic timer.
            e.expiry_tick = jiffies_ + e.interval_ticks;
            insert(e);
        }
    }
    return count;
}

inline int TimerWheel::cascade(int from_level) {
    int count = 0;
    if (from_level == 1) {
        std::size_t s = (jiffies_ >> 8) & (kL1Slots - 1);
        Slot local;
        local.swap(l1_[s]);
        for (auto &e : local) {
            entries_.erase(e.id);
            insert(e);  // re-insert into l0
        }
        count = static_cast<int>(local.size());
    } else if (from_level == 2) {
        std::size_t s = (jiffies_ >> 14) & (kL2Slots - 1);
        Slot local;
        local.swap(l2_[s]);
        for (auto &e : local) {
            entries_.erase(e.id);
            insert(e);
        }
        count = static_cast<int>(local.size());
    } else if (from_level == 3) {
        std::size_t s = (jiffies_ >> 20) & (kL3Slots - 1);
        Slot local;
        local.swap(l3_[s]);
        for (auto &e : local) {
            entries_.erase(e.id);
            insert(e);
        }
        count = static_cast<int>(local.size());
    }
    stats_.cascades += static_cast<uint64_t>(count);
    return count;
}

inline int TimerWheel::tick() {
    ++jiffies_;

    // Cascade from higher levels when the corresponding boundary is crossed.
    if ((jiffies_ & (kL0Slots - 1)) == 0) {
        cascade(1);
        if ((jiffies_ & (kL0Slots * kL1Slots - 1)) == 0) {
            cascade(2);
            if ((jiffies_ & (kL0Slots * kL1Slots * kL2Slots - 1)) == 0) {
                cascade(3);
            }
        }
    }

    std::size_t s = jiffies_ & (kL0Slots - 1);
    return fire_slot(l0_[s]);
}

inline int TimerWheel::advance(uint64_t n) {
    int total = 0;
    for (uint64_t i = 0; i < n; ++i)
        total += tick();
    return total;
}

}  // namespace hft
