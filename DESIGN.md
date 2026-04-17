# SPSC Message Bus — Design Notes

## Overview

A lock-free, single-producer single-consumer (SPSC) message bus over POSIX
shared memory.  The hot path contains no system calls, no locks, and no
`seq_cst` fences — only two atomic loads and one atomic store per message.

---

## Key Design Decisions

### 1. Power-of-2 ring buffer, bitwise index wrap

The ring capacity must be a power of two.  This lets index wrapping use a
single bitwise AND (`pos & (capacity - 1)`) instead of a modulo, which is
cheaper on every iteration of the hot path.

### 2. `write_pos` and `read_pos` on separate cache lines

```
ControlBlock
  [cache line 0]  magic, capacity, slot_stride, max_msg_size   (read-only)
  [cache line 1]  write_pos   ← written by producer only
  [cache line 2]  read_pos    ← written by consumer only
```

If both counters shared a cache line the cores would ping-pong that line on
every message, serialising the two sides.  Putting them on separate lines means
each core owns its line exclusively and only reads the other side's line when
checking for full/empty.

### 3. Cached remote position

Each side keeps a local copy of the other side's counter:

- Producer caches `read_pos`  → only re-reads it when the ring appears full.
- Consumer caches `write_pos` → only re-reads it when the ring appears empty.

This cuts cross-core coherence traffic by up to `capacity` times under
sustained throughput: in the common case the remote cache line is never
touched.

### 4. Slots padded to cache-line stride

```
slot_stride = round_up(sizeof(SlotHeader) + max_msg_size, 64)
```

Without alignment, adjacent slots can share a cache line.  If the producer
writes slot N while the consumer reads slot N-1, both cores fight over the
same line — false sharing.  Padding each slot to a cache-line boundary
eliminates this.

### 5. `acquire` / `release` ordering only

- `write_pos.store(release)` — guarantees slot payload is visible before the
  counter advances.
- `write_pos.load(acquire)` (consumer) — guarantees the counter is visible
  before the slot contents are read.

No `seq_cst`, no explicit `mfence`.  On x86, `release` compiles to a plain
store (TSO makes it free) and `acquire` compiles to a plain load.  On ARM,
both emit a single `stlr` / `ldar`.

### 6. `_mm_pause()` in spin loops

When the ring is full or empty the spinning side calls `_mm_pause()` (or
`yield` on ARM) each iteration.  This reduces pipeline back-pressure on
hyper-threaded cores and lowers power consumption without adding latency.

### 7. POSIX `shm_open` + `mmap`

Shared memory is created once with `shm_open` + `ftruncate` + `mmap`.  After
that, message passing involves no system calls — both processes read and write
the mapped region directly.  The kernel is only involved on page faults (first
access) and at teardown.

### 8. Magic word as an init fence

The producer writes all control-block fields first, then writes the `magic`
word last with a `release` fence.  The consumer spins on `magic` and issues an
`acquire` fence once it becomes valid.  This ensures the consumer never sees a
partially-initialised control block without requiring any out-of-band
synchronisation mechanism.

---

## Memory Layout

```
[ ControlBlock          192 B  (3 × 64-byte cache lines) ]
[ Slot 0                stride B                         ]
[ Slot 1                stride B                         ]
  ...
[ Slot N-1              stride B                         ]

stride = round_up(8 + max_msg_size, 64)

Slot:
  [0..3]   uint32_t size     — payload bytes written
  [4..7]   uint32_t _pad
  [8..]    uint8_t  payload[max_msg_size]
```

---

## Ordering Invariants

```
Producer                          Consumer
────────                          ────────
write slot[wp].size               load read_pos  (acquire)
write slot[wp].payload            ↓
store write_pos  (release)  →→→  load write_pos (acquire)
                                  read slot[rp].size
                                  read slot[rp].payload
load read_pos    (acquire)  ←←←  store read_pos (release)
↑
(check for full)
```

The release/acquire pair on each counter acts as the happens-before edge that
makes slot contents visible across cores without additional fences.

---

## Timestamping Architecture

See **[TIMING.md](TIMING.md)** for full detail on:
- RDTSCP vs `clock_gettime` vDSO — when to use each
- Why `clock_gettime` does not syscall on Linux (vDSO proof)
- Benchmark measurement overhead (~10 cycles per message from two RDTSCP stamps)
- TSC calibration against `CLOCK_MONOTONIC_RAW`
- TSC ↔ `CLOCK_REALTIME` correlation for MiFID II audit timestamps
- Background recalibration (software fallback without PTP NIC)
- What HFT firms actually use at each pipeline layer

---

## Performance Tuning Tips

| Knob | Recommendation |
|---|---|
| CPU affinity | Pin producer and consumer to separate physical cores (`taskset` or `pthread_setaffinity_np`). Avoid hyper-thread siblings for lowest latency. |
| CPU isolation | Boot with `isolcpus=` to prevent the scheduler from placing other tasks on the dedicated cores. |
| Ring capacity | Size it so the ring is never full under sustained load. A full ring forces the producer to re-read `read_pos`, adding a cache-miss. |
| Message size | Smaller messages → more iterations of the hot path per byte. Larger messages → more memcpy cost per message. Tune to your workload. |
| `CLOCK_MONOTONIC_RAW` | Use for latency measurement if `CLOCK_MONOTONIC` shows NTP-induced jitter. |
| Huge pages | `mmap` the shared segment with `MAP_HUGETLB` to reduce TLB pressure for large rings. |

---

## Limitations

- **Single producer, single consumer only.**  The design assumes exclusive
  ownership of `write_pos` and `read_pos` respectively.  Adding a second
  producer or consumer requires a different algorithm (e.g. MPMC with sequence
  numbers).
- **Fixed maximum message size.**  The slot stride is set at creation time.
  Variable-length messages beyond `max_msg_size` are silently rejected.
- **x86 / ARM only.**  `_mm_pause()` / `yield` and the TSO memory model
  assumptions are architecture-specific.  Other ISAs would need review.
- **No flow control beyond spinning.**  A persistently full ring will keep the
  producer spinning indefinitely.  Add a timeout or backpressure signal if
  needed.
- **Producer must start first.**  `SharedBus::open()` throws immediately on
  `ENOENT` if the producer has not yet called `create()`.  No retry logic exists.

---

## Roadmap

### Phase 1 — Code Quality (immediate)

| Task | Detail |
|---|---|
| Fix C++ standard | `CMakeLists.txt` declares C++20; CLAUDE.md requires C++17. Change `CMAKE_CXX_STANDARD` to 17. |
| Add `.clang-format` | Google or LLVM base style, 4-space indent, 100-column limit. |
| Add `.clang-tidy` | Enable `cppcoreguidelines-*`, `modernize-*`, `performance-*`, `readability-*`. Wire into CMake via `CMAKE_CXX_CLANG_TIDY`. |
| Sanitizer build targets | `cmake -DSANITIZE=asan` → `-fsanitize=address,undefined`. `cmake -DSANITIZE=msan` → `-fsanitize=memory`. Always run unit tests under ASan before merge. |
| `perf` profiling preset | `cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo` — keeps debug symbols for `perf report` / flame graphs. Without this, `perf` shows raw addresses. |
| Unit tests | Add test binary (Catch2 or Google Test) covering the cases below. |

**Required unit test cases:**

| Test | Edge case |
|---|---|
| Basic push/pop round-trip | zero-length guard, max_msg_size boundary |
| Ring index wraps correctly | capacity-1 and capacity slots in flight |
| Ring full → `try_push` returns false | exactly at capacity boundary |
| Ring empty → `try_pop` returns 0 | first pop on fresh bus |
| Non-power-of-2 capacity → throws | 0, 1, 3, 5, 1023 |
| `max_msg_size=0` → throws | |
| Message larger than `max_msg_size` → rejected | size == max+1 |
| Consumer-first open → spins until producer inits | via two threads |
| Ordering: all sent bytes arrive in order | 1 M message stress pass |

---

### Phase 2 — Recovery & Fault Tolerance

**A. Producer crash detection**

Problem: if the producer process dies, `write_pos` freezes and the consumer
spins forever on an empty ring.

Fix: add `producer_pid` and `producer_heartbeat_ts` fields to `ControlBlock`.
The consumer periodically checks `kill(producer_pid, 0)` — if it returns
`ESRCH`, the producer is dead.  The consumer can then abort, reconnect, or
raise an alert.

**B. Consumer crash detection**

Problem: if the consumer process dies, `read_pos` freezes, the ring fills, and
the producer spins forever on a full ring.

Fix: add `consumer_pid` and `consumer_heartbeat_ts` to `ControlBlock`.  The
producer detects the dead consumer and can choose to drop messages (lossy mode)
or abort.

**C. Stale shared-memory segment on restart**

Problem: a leftover shm segment from a previous run passes the `magic` check
but contains stale `write_pos`/`read_pos` values or old message data.

Fix: add a `session_id` (random 64-bit value written at `create()`) to
`ControlBlock`.  Both sides record the expected `session_id`; a mismatch on
`open()` triggers an error or re-creation.

**D. Consumer-first startup**

Problem: `shm_open(O_RDWR)` in `open()` fails immediately with `ENOENT` if the
producer has not yet called `create()`.

Fix: wrap `shm_open` in a retry loop with a configurable timeout.  Also guard
the `fstat` size check against the window between `shm_open` and `ftruncate`
on the producer side (size may be 0 momentarily).

---

### Phase 3 — Production Hardening (future)

| Item | Rationale |
|---|---|
| `mlock()` the shm segment | Prevent page faults on the hot path under memory pressure. |
| `MAP_HUGETLB` | Reduce TLB pressure for large rings (≥ 2 MB). |
| CPU affinity helpers | Expose `pthread_setaffinity_np` wrappers in the demo; pin producer and consumer to isolated physical cores. |
| `CLOCK_MONOTONIC_RAW` | Eliminate NTP-induced jitter from latency measurements. |
| Backpressure signal via `eventfd` | Allow a full-ring producer to sleep instead of burning a core, for mixed-criticality deployments. |
| Lossy / overwrite mode | Optional flag to let the producer overwrite the oldest unread slot; requires per-slot generation counter in `SlotHeader._pad`. |
