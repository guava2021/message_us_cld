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
