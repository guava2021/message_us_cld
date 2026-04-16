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
- CI/CD pipeline design (GitHub Actions) → `.github/workflows/ci.yml` created
- Local git hooks → `scripts/hooks/pre-commit`, `pre-push`, `install-hooks.sh` created

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

## Ring Sizing & Cache Impact

### Key insight: occupancy drives latency, not capacity
- `capacity` is just the ceiling; `write_pos - read_pos` (occupancy) is the working set.
- If occupancy stays at 3–5 slots, only those slots are hot in L1/L2. The rest of the
  ring is cold and irrelevant.
- A cache miss costs ~100 ns. High occupancy → producer writes to cold lines → latency spikes.

### Cache math (64-byte messages)
- `slot_stride = round_up(8 + 64, 64) = 128 bytes` — 50% padding waste
- L1 (32 KB) fits 256 slots; L2 (256 KB) fits 2,048 slots; L3 (~12 MB) fits ~96K slots
- Current default 64K ring @ 128 B/slot = 8 MB → spills out of L2

### Slot stride waste
- Design `msg_size` to be `(N × 64) - 8` to fill slots cleanly and halve cache footprint:
  - 56 B msg → 64 B stride (0% waste)
  - 120 B msg → 128 B stride (0% waste)
  - 64 B msg → 128 B stride (50% waste ← current default)

### Practical sizing rule
- [ ] Never fill the ring in steady state — size for worst burst, not average load
- [ ] Start with 1K–4K slots; increase only if drops appear during bursts
- [ ] Keep occupancy < 10% of capacity as a health target
- [ ] Tune msg_size to eliminate stride padding

---

## Performance Measurement

### Layer 1 — built-in benchmarks (already present)
- `bench_thread.cpp`: single-process two-thread throughput + P50/P90/P99/P99.9/max latency
- `consumer_demo.cpp` + `producer_demo.cpp`: cross-process IPC latency
- Run via `cmake --build build --target run_bench` / `run_ipc`

### Layer 2 — `perf stat` (hardware counters)
```bash
perf stat -e cache-misses,cache-references,L1-dcache-load-misses,\
LLC-load-misses,branch-misses,instructions,cycles \
./build/bench 10000000 64 0 2
```
Key counters and what they reveal:

| Counter | What it means |
|---|---|
| `L1-dcache-load-misses` | Ring working set too large for L1 |
| `LLC-load-misses` | Severe cache thrashing — ring spills L3 |
| `instructions/cycles` (IPC < 2.0) | Memory-bound stalls |
| `branch-misses` | Mispredicted empty/full checks |

### Layer 3 — flame graphs
```bash
perf record -g ./build/bench 10000000 64 0 2
perf report
```
Requires `RelWithDebInfo` build to map addresses to function names.

### CMake build presets (Phase 1)
| Preset | Command | Use for |
|---|---|---|
| Release | `-DCMAKE_BUILD_TYPE=Release` | Throughput benchmarks |
| RelWithDebInfo | `-DCMAKE_BUILD_TYPE=RelWithDebInfo` | `perf` profiling / flame graphs |
| ASan | `-DSANITIZE=asan` | Memory safety, run all unit tests |
| MSan | `-DSANITIZE=msan` | Uninitialised read detection |

### Open questions on performance
- [ ] What is the actual P99.9 target? (sub-1µs intra-process? sub-5µs IPC?)
- [ ] Should latency histogram be percentile-sampled or full HDR histogram
      (HdrHistogram avoids allocation on hot path)?
- [ ] Add occupancy sampling to bench: record `write_pos - read_pos` every N
      messages to build an occupancy histogram alongside the latency histogram.

---

## Phase 3 — Production Hardening (future ideas)

- `mlock()` shm segment — prevent page faults under memory pressure
- `MAP_HUGETLB` — reduce TLB pressure for large rings (≥ 2 MB)
- CPU affinity helpers — `pthread_setaffinity_np` wrappers, pin to isolated cores
- `CLOCK_MONOTONIC_RAW` — eliminate NTP jitter from latency measurements
- `eventfd` backpressure signal — producer sleeps instead of burning a core
- Lossy overwrite mode — per-slot generation counter, producer overwrites oldest

---

## CI/CD & Local Hooks

### Recommended CI tool
- **GitHub Actions** for cloud CI (free, Linux-native, matrix builds)
- **Self-hosted runner** on dedicated hardware if perf benchmarks need to be in CI
  (cloud runners have noisy neighbors — unreliable for latency regression gates)

### Pipeline stages (GitHub Actions)
```
push / PR
  ├─► [parallel] clang-format gate (fast, staged files only)
  ├─► [parallel] build matrix: Debug / Release / RelWithDebInfo × clang + gcc
  ├─► [after build] clang-tidy (needs compile_commands.json)
  ├─► [parallel] unit tests — ASan build
  ├─► [parallel] unit tests — MSan build
  ├─► [on merge to main] benchmark smoke (throughput regression > 10% = fail)
  └─► [after tests] /dev/shm leak check
```

### Local git hooks (committed, installed via script)
Scripts live in `scripts/hooks/`, installed by `scripts/install-hooks.sh`.

| Hook | Trigger | What runs | Speed |
|---|---|---|---|
| `pre-commit` | `git commit` | clang-format on staged files | <1s |
| `pre-push` | `git push` | clang-tidy + ASan tests + shm cleanup | ~30s |

Install once after cloning:
```bash
bash scripts/install-hooks.sh
```

### Can local machine replace CI?
| Stage | Local | CI | Winner |
|---|---|---|---|
| clang-format | Yes | Yes | Either |
| clang-tidy | Yes | Yes | Either |
| ASan/MSan tests | Yes | Yes | Either |
| `perf` profiling | **Yes** | No (blocked) | **Local** |
| CPU pinning | **Yes** | No | **Local** |
| Benchmark regression | **Yes** (less noise) | Unreliable | **Local** |
| Multi-platform matrix | No | **Yes** | **CI** |

Local = better for performance work. CI = better for correctness matrix and gating merges.

### Required local tools
```bash
sudo apt install clang clang-format clang-tidy cmake ninja-build \
    linux-tools-common linux-tools-$(uname -r)
# Allow perf without root
sudo sysctl kernel.perf_event_paranoid=1
```

### Open questions on CI/CD
- [ ] Self-hosted runner needed for benchmark regression gate?
- [ ] Add MSan to pre-push or keep it CI-only? (MSan requires clang, not all devs have it)
- [ ] Benchmark baseline stored in repo (JSON) or fetched from CI artifact?
- [ ] Add a `pre-receive` server-side hook to enforce format on the remote?

---

## Ideas Parking Lot (not yet evaluated)

- MPMC upgrade path: sequence numbers per slot, CAS on write_pos
- Shared memory persistence across reboots (`MAP_SHARED` + file-backed mmap
  instead of `shm_open`) — useful for replaying missed messages after restart
- Zero-copy API: expose a `claim()` / `commit()` interface so callers write
  directly into the slot without an intermediate buffer
- Variable-length messages: store a level of indirection (offset table) in a
  separate shm region; hot ring carries only fixed-size headers + offsets
