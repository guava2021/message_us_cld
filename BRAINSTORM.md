# SPSC Bus — Brainstorm & Open Questions

This file captures raw ideas, open questions, and half-formed thoughts from
design sessions.  When an idea matures it gets promoted to DESIGN.md and
removed here.

---

## Session: 2026-04-16

### Topics covered

- Sequence numbers
- Startup ordering (producer-first vs consumer-first)
- Code quality gaps (C++17, clang-format, clang-tidy, sanitizers, unit tests)
- Recovery & fault tolerance phases
- Backpressure monitoring and HFT strategies
- Ring sizing and cache impact on latency
- Performance measurement (perf, sanitizers, built-in benchmarks)

---

## Open Questions

### Sequence numbers
- **Conclusion:** not needed for correctness in SPSC. `write_pos` is already an
  implicit sequence number. The existing `SlotHeader._pad` could hold a
  truncated seqno for debugging only — zero hot-path benefit.
- **Deferred:** revisit if lossy/overwrite mode is added (generation counter
  needed to detect consumer being lapped).

### Startup ordering
- **Conclusion:** currently producer-must-start-first. `open()` throws
  immediately on `ENOENT`.
- **Fix needed (Phase 2-D):** retry loop with configurable timeout in `open()`.
  Also guard the `fstat` size==0 window between producer's `shm_open` and
  `ftruncate`.

---

## Backpressure

### Monitoring
- Measure ring occupancy: `write_pos - read_pos` (already available cheaply).
- Add a stats block to `ControlBlock` on its own cache line, `relaxed` atomics:
  - `drops` — messages rejected (ring full)
  - `hwm_occupancy` — high-water mark slot count ever seen
  - `times_above_75` — how often occupancy crossed 75% of capacity
  - `times_full` — how often ring was completely full
- A third monitoring process can `open()` the same shm and read stats without
  touching hot-path cache lines.

### HFT strategies under high backpressure

| Strategy | Use case | Complexity | Status |
|---|---|---|---|
| Drop + stats counter | Market data, non-critical msgs | Low | To implement |
| Overwrite oldest (lossy ring) | Tick feeds, snapshots | Medium | To design |
| Two-tier priority rings | Mixed criticality (fills vs. data) | Medium | To design |
| Circuit breaker on occupancy threshold | Order rate control | Medium | To design |
| Consumer-side batching | High-throughput, latency-tolerant path | Low | To design |

### Open questions on backpressure
- [ ] What message types in our system are droppable vs. must-deliver?
      (fills/acks = must-deliver; market data ticks = droppable)
- [ ] Lossy overwrite: use `SlotHeader._pad` as generation counter?
      Producer writes `(write_pos >> log2(capacity))`, consumer checks for mismatch.
- [ ] Two-tier rings: single `SharedBus` with priority field in `SlotHeader`,
      or two completely separate shm segments?  Separate segments avoids
      head-of-line blocking on the high-priority path.
- [ ] Circuit breaker threshold: hardcoded (75%) or configurable in
      `ControlBlock` at `create()` time?
- [ ] Should the stats block be in the same shm segment or a separate one?
      Separate avoids a monitoring process keeping the segment alive after
      producer/consumer exit.

---

## Recovery & Fault Tolerance (Phase 2)

### A. Producer crash detection
- Add `producer_pid` + `producer_heartbeat_ts` to `ControlBlock`.
- Consumer checks `kill(producer_pid, 0)` — `ESRCH` means dead.
- Open question: what should consumer do on detection? Options:
  - [ ] Drain remaining messages then exit cleanly
  - [ ] Raise SIGTERM to itself / alert ops
  - [ ] Block and wait for producer to restart (needs session_id check)

### B. Consumer crash detection
- Add `consumer_pid` + `consumer_heartbeat_ts` to `ControlBlock`.
- Producer detects dead consumer; options:
  - [ ] Switch to drop mode (lossy) and keep running
  - [ ] Abort and alert
  - [ ] Wait for consumer to restart

### C. Stale shm on restart
- Add `session_id` (random 64-bit) written at `create()`.
- Consumer verifies `session_id` matches expected value on `open()`.
- Open question: who is responsible for `shm_unlink`?  Currently consumer
  unlinks at exit — but if consumer crashes, segment is never cleaned up.
  Consider: producer unlinks on `create()` after verifying no live session_id.

### D. Consumer-first startup
- Retry loop on `ENOENT` with configurable timeout (default: infinite spin,
  parameterisable).
- Also need: re-check `st_size > 0` after `fstat` to handle the
  `shm_open`→`ftruncate` gap on producer side.

---

## Phase 3 — Production Hardening (future ideas)

- `mlock()` shm segment — prevent page faults under memory pressure
- `MAP_HUGETLB` — reduce TLB pressure for large rings (≥ 2 MB)
- CPU affinity helpers — `pthread_setaffinity_np` wrappers, pin to isolated cores
- `CLOCK_MONOTONIC_RAW` — eliminate NTP jitter from latency measurements
- `eventfd` backpressure signal — producer sleeps instead of burning a core
- Lossy overwrite mode — per-slot generation counter, producer overwrites oldest

---

## Ideas Parking Lot (not yet evaluated)

- MPMC upgrade path: sequence numbers per slot, CAS on write_pos
- Shared memory persistence across reboots (`MAP_SHARED` + file-backed mmap
  instead of `shm_open`) — useful for replaying missed messages after restart
- Zero-copy API: expose a `claim()` / `commit()` interface so callers write
  directly into the slot without an intermediate buffer
- Variable-length messages: store a level of indirection (offset table) in a
  separate shm region; hot ring carries only fixed-size headers + offsets
