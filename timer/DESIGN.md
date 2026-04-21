# Timer Design Document

## Problem Statement

HFT systems need timers at multiple granularities:
- **Coarse (1 ms – seconds)**: order timeouts, heartbeats, reconnect delays, market session boundaries
- **Fine (10 µs – 1 ms)**: quote throttle windows, fill latency watchdogs, burst limiters

Requirements:
- Low-latency callback dispatch on the hot path
- Scalable to thousands of concurrent timers (order-timeout per live order)
- No busy-spin on the coarse path (don't waste a core sleeping)
- Deterministic cancel (no dangling callbacks after `cancel()` returns)

---

## Architecture: Two-Layer Design

```
┌─────────────────────────────────────────┐
│  EpollTimer  (sleep / wake mechanism)   │
│  timerfd + epoll → wakes at deadline    │
│  min-heap, O(log N) insert/cancel       │
└────────────────┬────────────────────────┘
                 │ tick() every 1 ms
┌────────────────▼────────────────────────┐
│  TimerWheel  (dispatch data structure)  │
│  4-level hierarchical wheel             │
│  O(1) insert/cancel/fire                │
└─────────────────────────────────────────┘
```

`EpollTimer` owns the OS interaction. `TimerWheel` is a pure data structure with no OS calls.
They can be used independently or composed.

---

## EpollTimer Design

### Mechanism: single timerfd + epoll

One `timerfd` (CLOCK_MONOTONIC) is registered in one `epoll` instance.
When the earliest pending deadline arrives, the kernel wakes `epoll_wait`.

**Why one timerfd instead of one per timer?**

Each `timerfd` is a file descriptor. The naive approach — one fd per timer — hits the process fd limit fast (default 1024 soft, 65536 hard) and turns `epoll` into an O(N) scan of N fds per wakeup. A single fd that is rearmed to the next deadline keeps epoll O(1) regardless of timer count.

### Deadline tracking: min-heap

All pending timers sit in a `std::priority_queue` (min-heap) ordered by absolute expiry timestamp. On each wakeup, expired entries are drained front-to-back.

**Why a min-heap?**

- Insert: O(log N)
- Find minimum: O(1) — needed to rearm timerfd to the next deadline
- Pop minimum: O(log N)

For typical HFT coarse-timer counts (tens to low hundreds), log N is negligible.

**Why not a sorted `std::set` or `std::map`?**

`std::set` and `std::map` are red-black trees: same asymptotic complexity but higher constant factor (pointer chasing, worse cache locality) compared to the contiguous array backing `priority_queue`.

### Cancel: lazy deletion

`cancel(id)` removes the entry from `active_` (an `unordered_map`) immediately.
The stale entry remains in the heap but is skipped when popped: if `active_` has no matching id, or the stored expiry doesn't match, the entry is discarded.

**Why lazy?**

A true O(1) heap remove requires an index structure (e.g. position map into the heap array). That adds memory and complexity for a case (cancel) that is off the hot path. Lazy deletion keeps the heap simple and the cancel path to a single hash-map erase.

**Corner case — cancel just before fire:**
If `cancel()` is called between the `timerfd` waking and `drain_expired()` running, the entry is already removed from `active_`. `drain_expired()` will pop it from the heap, find no matching active entry, and skip it silently. The callback is never invoked. This is the correct behaviour.

### Periodic timer catch-up

When a periodic timer fires, the next expiry is computed as:
```cpp
e.expiry_ns += e.interval_ns;
while (e.expiry_ns <= now) e.expiry_ns += e.interval_ns;
```

If the system was busy (or the process was preempted), this skips missed intervals rather than firing a burst of catch-up callbacks. This matches the behaviour of Linux `timerfd` with `TFD_TIMER_ABSTIME` and is the right default for HFT: a burst of stale heartbeats or timeout checks is harmful.

**Alternative: fire all missed intervals** — useful for accounting/audit logs where every event matters. Not implemented; easy to add as a policy flag.

### Clock choice: CLOCK_MONOTONIC

`CLOCK_MONOTONIC` never jumps backward and is unaffected by NTP adjustments or leap seconds. `CLOCK_REALTIME` can jump, causing timers to fire immediately (forward jump) or be delayed indefinitely (backward jump).

**Corner case — clock monotonicity at startup:**
CLOCK_MONOTONIC is monotonic within a boot. It does not survive reboots. This is fine — `EpollTimer` is a runtime object.

---

## TimerWheel Design

### Mechanism: hierarchical timer wheel

Four levels of slot arrays, each representing a coarser time resolution:

| Level | Slots | Granularity | Range |
|-------|-------|-------------|-------|
| L0    | 256   | 1 tick      | 256 ticks  |
| L1    | 64    | 256 ticks   | ~16K ticks |
| L2    | 64    | 16K ticks   | ~1M ticks  |
| L3    | 64    | 1M ticks    | ~64M ticks |

At 1 ms/tick: L0=256 ms, L1=~16 s, L2=~17 min, L3=~18 hours.

A new timer is placed in the lowest level that can represent its expiry. Slot index is computed by masking the expiry tick with the level's bitmask — no division, pure bit ops.

### Insert: O(1)

```cpp
remaining = expiry_tick - jiffies_;
if      (remaining < 256)        → L0, slot = expiry & 0xFF
else if (remaining < 256*64)     → L1, slot = (expiry >> 8) & 0x3F
else if (remaining < 256*64*64)  → L2, slot = (expiry >> 14) & 0x3F
else                             → L3, slot = (expiry >> 20) & 0x3F
```

### Cancel: O(1)

A `Location` map (TimerID → level + slot + list iterator) allows direct removal from the doubly-linked slot list in O(1). This is the key difference from the min-heap: cancel is truly O(1) with no deferred work.

**Why `std::list` per slot instead of a vector?**

Iterator stability under insert/erase. We store iterators in the Location map. `std::vector` invalidates iterators on any push; `std::list` does not.

**Performance note:** `std::list` has poor cache locality (each node heap-allocated separately). For very high timer counts, a pool-allocated intrusive list (using the existing `MemoryPool` component) would eliminate the heap pressure. Left as a Phase 2 improvement.

### Cascading

When the tick counter crosses a level boundary (every 256 L0 ticks, every 64 L1 ticks, etc.), all entries in the current L1/L2/L3 slot are re-inserted into finer-grained levels. This is standard timer wheel cascading.

**Corner case — cascade during fire:**
Cascaded entries are re-inserted via `insert()`, which adds them to lower-level slots. Those slots may be iterated in the same `tick()` call if their L0 slot index happens to match `jiffies_ & 0xFF`. This is handled correctly because `tick()` fires L0 *after* cascading.

**Corner case — timer expiry exactly on a cascade boundary:**
A timer scheduled for exactly tick 256 lands in L1 slot 1. When tick 256 is reached, cascade(1) runs first, moving the entry to L0 slot 0, which then fires in `fire_slot(l0_[0])`. Correct.

### Re-entrancy: callbacks may reschedule

`fire_slot()` swaps the slot list out before iterating:
```cpp
Slot local;
local.swap(slot);       // slot is now empty
for (auto& e : local) { e.cb(); ... }  // new schedules go into now-empty slot
```

A callback calling `schedule()` during its own execution inserts into the wheel safely — it never modifies the list being iterated.

---

## Alternatives Considered

### Alternative 1: One timerfd per timer

**Pro:** Simple — no heap, no rearm logic.  
**Con:** O(N) fd consumption; `epoll` becomes an N-fd scan; hits fd limits at scale.  
**Verdict:** Rejected.

### Alternative 2: `SIGALRM` / `setitimer`

**Pro:** No fd required.  
**Con:** Signal delivery is async and unsafe in multi-threaded programs (signal can land on any thread). No sub-ms precision. Cannot have multiple independent timers.  
**Verdict:** Rejected for production use.

### Alternative 3: `io_uring` + `IORING_OP_TIMEOUT`

**Pro:** Lower syscall overhead; composable with network I/O already in the ring.  
**Con:** Requires kernel ≥ 5.4; more complex setup; not yet standard in most HFT environments.  
**Verdict:** Future upgrade path if the project adopts `io_uring` for networking.

### Alternative 4: TSC busy-spin only

**Pro:** Sub-µs precision; no kernel involvement on the hot path.  
**Con:** Burns an entire core spinning; power/thermal impact; not suitable for coarse timers.  
**Verdict:** Right for the final sub-100 µs stretch (as a Phase 3 hybrid), not as a general timer.

### Alternative 5: Single flat timer wheel (no hierarchy)

**Pro:** Simpler code.  
**Con:** Large range requires huge slot array (64K slots for 64s at 1ms) wasting memory and L1 cache; or coarse granularity. Hierarchical design is the standard solution.  
**Verdict:** Rejected in favour of hierarchical wheel.

### Alternative 6: `std::set<pair<expiry, id>>` instead of min-heap

**Pro:** O(log N) insert/erase; true O(1) cancel (erase by iterator).  
**Con:** Red-black tree node per entry (pointer chasing, allocator pressure); worse cache behaviour than heap array.  
**Verdict:** Not worth the cache penalty for the same asymptotic complexity.

---

## Corner Cases

| Case | Behaviour |
|------|-----------|
| `cancel()` called between `epoll_wait` wakeup and `drain_expired()` | Entry removed from `active_`; heap pop finds no match; callback not invoked. Correct. |
| `cancel()` on already-fired one-shot | `active_` lookup misses; returns `false`. Safe. |
| `cancel()` on already-cancelled id | Same as above. Returns `false`. |
| Periodic timer fires while system is suspended / preempted | Catch-up loop skips missed intervals; fires once with the next aligned deadline. No burst. |
| Two timers with identical expiry | Both sit in heap; both fire in the same `drain_expired()` call. Order between them is unspecified (heap tie-breaking by insertion order is an implementation detail). |
| `schedule(0, cb)` — zero delay | `EpollTimer`: fires on next `run_once()` since `now + 0 <= now`. `TimerWheel`: clamped to 1 tick. Both correct. |
| Timer callback schedules a new timer | `EpollTimer`: new entry pushed to heap; rearm happens after drain loop. `TimerWheel`: list-swap in `fire_slot` prevents iterator invalidation. Both safe. |
| Timer callback cancels another timer | `EpollTimer`: removes from `active_`; lazy heap skip handles it. `TimerWheel`: `cancel()` erases from Location map and list; safe mid-drain since we operate on a swapped-out local list. |
| `stop()` called from signal handler | `stop()` only does an atomic store — async-signal-safe. `run()` exits on the next `run_once()` iteration. |
| `TimerWheel` overflow (expiry > L3 range ~64M ticks) | Slot index wraps due to bitmask. Timer fires at the wrong tick. **Caller must not schedule beyond the wheel's range.** Add a runtime assert for debug builds. |
| `timerfd` overrun > 1 | Kernel coalesces missed ticks into the 64-bit read value. `EpollTimer` records `overruns += value - 1` in stats. Only one `drain_expired()` is called (correct — `now_ns()` captures the real wall time). |

---

## What Is Not Implemented (Phase 2 Suggestions)

- **Intrusive pool-allocated list** in `TimerWheel` slots — eliminate per-node heap allocation
- **TSC hybrid mode** — `EpollTimer` wakes at ~1 ms, then spins on TSC for the final <100 µs
- **Thread-safe `schedule()` / `cancel()`** — add a `pending_` SPSC queue; timer thread drains it at the top of each `run_once()`; avoids a mutex on the hot path
- **`io_uring` backend** — replace `timerfd` + `epoll_wait` with `IORING_OP_TIMEOUT` for unified I/O + timer ring
- **Wheel range assert** — `static_assert` or runtime check that `delay_ticks < kMaxWheelRange`
