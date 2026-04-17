# Timestamping Architecture (`tsc_clock.hpp`)

## RDTSCP vs `clock_gettime` — choosing the right tool

Both are used in production HFT. The choice depends on latency budget and
whether a wall-clock stamp is needed directly.

**`clock_gettime` via vDSO**

On Linux, `clock_gettime(CLOCK_REALTIME/MONOTONIC/TAI)` does *not* always
syscall.  The kernel maps a vDSO page that userspace reads directly:
- Cost: ~20–40 ns (two memory reads + arithmetic), no kernel entry
- Returns wall-clock time directly — no TSC↔UTC conversion step needed
- Used on the hot path at firms where 20–40 ns is within the latency budget
  and an exchange-facing UTC timestamp is required per message

**RDTSCP**

`RDTSCP` reads the hardware TSC register directly:
- ~5 cycles (~2 ns), no syscall, no vDSO, no kernel involvement
- Serializing: CPU retires all preceding instructions before reading
- Returns a raw tick counter — requires offline conversion to wall time
- Preferred when every nanosecond counts (co-lo market making, internal
  pipeline profiling) or when measuring deltas rather than absolute time
- Prerequisite CPU flags: `constant_tsc` + `nonstop_tsc` (all modern Intel/AMD,
  guaranteed since Intel Nehalem 2008)

**When to use each:**

| Situation | Choice |
|---|---|
| Internal delta (queue depth, strategy latency) | RDTSCP |
| Exchange-facing order timestamp, 20–40 ns acceptable | `clock_gettime(CLOCK_TAI)` vDSO |
| Sub-10 ns hot path, convert to UTC offline | RDTSCP + correlation pair |
| Regulatory audit log (MiFID II) | Either — both meet ~1 µs requirement |

This library uses RDTSCP on the critical path and defers wall-clock conversion
to the reporting path.  Switching to `clock_gettime(CLOCK_TAI)` is a valid
alternative if direct UTC stamps are needed and 20–40 ns overhead is acceptable.

---

## Why RDTSCP is preferred in the benchmark (not because of syscalls)

Both RDTSCP and `clock_gettime` vDSO are valid for cross-core delta measurement
— neither syscalls, and both read from the same underlying time source across
cores.  The reason `bench_thread.cpp` uses RDTSCP is purely **cost per stamp**:

| | RDTSCP | `clock_gettime` vDSO |
|---|---|---|
| Syscall | No | No |
| Cost per call | ~2–5 ns | ~20–40 ns |
| Cross-core delta valid | Yes (invariant TSC, Nehalem+) | Yes |

Each timestamp call adds directly to the measured latency.  At ~20–40 ns per
stamp, `clock_gettime` would inflate the reported SPSC latency and obscure the
actual bus cost.  RDTSCP at ~2–5 ns keeps measurement overhead negligible.

### Benchmark measurement overhead

Every message in `bench_thread.cpp` incurs **two** RDTSCP calls:

```cpp
// producer
msg->send_tsc = TscClock::now_tsc();  // ~5 cycles
prod.push(msg, msg_size);

// consumer
cons.pop(buf, msg_size);
lat_tsc[i] = TscClock::now_tsc() - buf->send_tsc;  // ~5 cycles
```

This means ~10 cycles (~4–5 ns) of stamping overhead is baked into every
reported latency sample.  Throughput is also slightly pessimistic: the
`now_tsc()` call inside the producer loop inflates `prod_elapsed_ns`, making
the reported msg/s lower than a production producer that does not stamp.

For a ~20–50 cycle push on a modern CPU, the stamp is a ~10–25% overhead.
This is acceptable for a latency benchmark — the overhead is small and
consistent.  For a pure throughput benchmark, remove the stamp from the
producer loop and measure `t1 - t0` around the entire batch.

---

## Proof that `clock_gettime` does not syscall (CppCon 2022, Nataly Rasovsky)

`strace -e clock_gettime date` produces no intercepted calls — `+++ exited with 0 +++`
with no `clock_gettime` lines.  strace intercepts at the syscall boundary; no
output means no syscall was executed.

The vDSO disassembly confirms why:

```asm
__clock_gettime2():
    mov  _rtld_global_ro@GLIBC_PRIVATE, %rax   ; load vDSO function pointer
    test %rax, %rax
    je   slow_path                             ; jump only if vDSO unavailable
    call *%rax                                 ; FAST PATH — pure userspace
    ret                                        ; syscall never reached

slow_path:
    ...
    syscall                                    ; only if vDSO not mapped (never in practice)
    ret
```

The `perf` sample counts on the fast-path instructions are non-zero; the
`syscall` instruction at the slow path has **zero samples** — the CPU was never
observed there.  The `je slow_path` branch is never taken on a normal Linux
system because the vDSO is always mapped.

---

## vDSO and CPU core pinning

The vDSO reads kernel timekeeping data protected by a seqlock.  If the OS
migrates the thread to a different core mid-read, the sequence numbers won't
match and the read retries — adding jitter.  More importantly, if you are
*benchmarking* `clock_gettime` itself, a core migration between two calls
inflates the measured cost artificially.

Pin benchmark threads to a specific core (`pthread_setaffinity_np` or
`taskset`) to eliminate this.  "Same CPU" here means same **core** — the
finest granularity of execution (not die or socket):

- **Core** — independent execution unit with its own registers, L1/L2 cache
- **Die** — physical silicon chip (may contain multiple cores)
- **Socket** — physical CPU package (may contain multiple dies)

`bench_thread.cpp` already does this via `pin_to_core()`.

---

## Critical path vs. offline conversion

```
Critical path (hot loop):
    send_tsc = TscClock::now_tsc()   // single RDTSCP — ~5 cycles
    recv_tsc = TscClock::now_tsc()
    latency_tsc = recv_tsc - send_tsc

Offline (logging / audit, not in the hot loop):
    latency_ns  = clk.delta_ns(latency_tsc)   // FP multiply
    wall_time   = clk.to_wall_ns(recv_tsc)    // two atomic loads + FP multiply
```

This separation is the same pattern used in production HFT systems: raw TSC
ticks are the source of truth; wall-clock conversion is deferred to the
reporting path and never pollutes tick-to-trade latency.

---

## TSC calibration

At startup `TscClock` busy-spins for 50 ms against `CLOCK_MONOTONIC_RAW` to
measure the TSC frequency in GHz (ticks per nanosecond):

```
CLOCK_MONOTONIC_RAW — same hardware oscillator as TSC, unaffected by NTP.
CLOCK_MONOTONIC     — same oscillator but NTP-slewed → would introduce drift
                      into the measured GHz value.
CLOCK_REALTIME      — wall clock, NTP-adjusted → worst choice for calibration.
```

Using `MONOTONIC_RAW` means `ghz_` is a stable physical constant for the
lifetime of the process, not subject to software correction.

---

## TSC ↔ CLOCK_REALTIME correlation (regulatory / MiFID II)

For audit trails and regulatory timestamps (MiFID II requires ~1 µs UTC
accuracy), `TscClock` maintains a `(tsc_base, wall_base_ns)` correlation pair:

```
Snap method (snap_correlation):
    clock_gettime(CLOCK_REALTIME, &t0)   // before RDTSCP
    tsc = RDTSCP()
    clock_gettime(CLOCK_REALTIME, &t1)   // after RDTSCP
    wall_ns = (t0 + t1) / 2             // midpoint minimises asymmetric jitter
```

`to_wall_ns(tsc) = wall_base_ns + (tsc - tsc_base) / ghz_`

---

## Background recalibration (software fallback)

NTP continuously slews `CLOCK_REALTIME`.  Over 5 s the drift between TSC-derived
wall time and true UTC is < 1 µs — acceptable when no PTP-disciplined NIC is
available.  A background thread calls `clk.recalibrate()` every 1–5 s to
refresh the correlation pair without touching the critical path:

```cpp
std::thread([&]() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        clk.recalibrate();   // atomic stores, not visible to hot path reads
    }
});
```

`recalibrate()` stores `wall_base_ns` first, then `tsc_base`, both with
`memory_order_release`.  A concurrent reader may see a slightly stale
`tsc_base` but never a sign-flipped delta (< 1 µs error, not a sign error).

**This is a software fallback for environments without hardware timestamping.**
Production HFT systems do not use a recalibration thread for wall-clock
anchoring — see below.

---

## What HFT firms actually use

| Layer | Timestamp source |
|---|---|
| Wire arrival | NIC hardware timestamp (PTP/IEEE 1588, Solarflare/Mellanox) — GPS-disciplined, sub-100 ns UTC accuracy |
| Kernel receive | `SO_TIMESTAMPING` with `SOF_TIMESTAMPING_HW_RX` |
| Userspace stages | RDTSCP — delta measurements only, never converted to wall time on the hot path |
| Regulatory audit (MiFID II) | `clock_gettime(CLOCK_TAI)` or NIC hardware timestamp — not TSC→wall conversion |

Key distinctions from the software approach above:

- **NIC hardware timestamps** are the authoritative UTC anchor.  The NIC is
  disciplined by a PTP grandmaster (GPS-locked stratum-1); no software
  recalibration thread is needed.
- **RDTSCP is used only for internal deltas** (e.g. strategy latency, queue
  depth timing).  Nobody converts TSC to wall time in the order path.
- **`CLOCK_TAI`** (not `CLOCK_REALTIME`) is used for regulatory logs.
  `CLOCK_TAI` has no leap-second discontinuities, which exchanges require for
  unambiguous audit timestamps.

The SPSC bus sits in the "userspace stages" layer.  RDTSCP gives sub-10 ns
resolution between producer and consumer stamps — sufficient to measure
internal pipeline latency down to single-digit nanoseconds.  The software
recalibration thread in this codebase is a pragmatic fallback for environments
without a PTP NIC.
