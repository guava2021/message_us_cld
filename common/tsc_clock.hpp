// tsc_clock.hpp — TSC-based clock with CLOCK_REALTIME correlation.
//
// Critical-path cost : one RDTSCP instruction (~5 cycles, no syscall).
// Wall-clock conversion: two atomic loads + one FP multiply (offline only).
//
// Design
// ──────
// RDTSCP reads the hardware TSC directly in userspace. To convert ticks to
// wall time, we record a (tsc, realtime_ns) correlation pair at startup and
// periodically refresh it from a background thread to absorb NTP drift.
//
// MiFID II / regulatory note
// ──────────────────────────
// The critical path stamps raw TSC ticks only. Wall-clock conversion is done
// after the fact (logging, order messages). This keeps the hot path to a
// single RDTSCP while still producing accurate UTC timestamps.
//
// Recalibration
// ─────────────
// TSC and CLOCK_REALTIME share the same oscillator but NTP adjusts REALTIME
// continuously. Over 5s the drift is <1µs — acceptable for regulatory
// timestamps (MiFID II requires ~1µs accuracy). Recalibrate every 1–5s from
// a background thread.
//
// Prerequisites: constant_tsc + nonstop_tsc CPU flags (all modern Intel/AMD).

#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>

namespace tsc {

// ── RDTSCP ───────────────────────────────────────────────────────────────────
// Serializes instruction retirement before reading — prevents the CPU from
// reordering the timestamp read relative to surrounding instructions.

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
static inline uint64_t rdtscp() noexcept {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    return (uint64_t(hi) << 32) | uint64_t(lo);
}

// ── TscClock ─────────────────────────────────────────────────────────────────

class TscClock {
   public:
    // Calibrate TSC frequency and snap an initial TSC↔REALTIME correlation.
    // Call once at process startup (takes ~50 ms for calibration).
    TscClock() : ghz_(calibrate_ghz()) {
        auto c = snap_correlation();
        tsc_base_.store(c.tsc, std::memory_order_relaxed);
        wall_base_ns_.store(c.wall_ns, std::memory_order_relaxed);
    }

    // ── critical path ─────────────────────────────────────────────────────────

    // Stamp the current TSC. No syscall — single RDTSCP instruction.
    static uint64_t now_tsc() noexcept { return rdtscp(); }

    // ── offline conversion ────────────────────────────────────────────────────

    // Convert a TSC reading to nanoseconds since Unix epoch (CLOCK_REALTIME).
    // Not for the critical path — intended for logging, order messages, audits.
    [[nodiscard]] int64_t to_wall_ns(uint64_t tsc) const noexcept {
        uint64_t base_tsc = tsc_base_.load(std::memory_order_acquire);
        int64_t base_wall = wall_base_ns_.load(std::memory_order_acquire);
        auto delta_ns     = static_cast<int64_t>(double(tsc - base_tsc) / ghz_);
        return base_wall + delta_ns;
    }

    // Convert a TSC delta (tsc_recv - tsc_send) to nanoseconds.
    [[nodiscard]] double delta_ns(uint64_t tsc_delta) const noexcept {
        return double(tsc_delta) / ghz_;
    }

    // TSC frequency in GHz (ticks per ns).
    [[nodiscard]] double ghz() const noexcept { return ghz_; }

    // ── recalibration (call from background thread every 1–5 s) ──────────────

    // Refresh the TSC↔REALTIME correlation to absorb NTP drift.
    // Thread-safe: uses atomic stores with release ordering.
    void recalibrate() noexcept {
        auto c = snap_correlation();
        // Store wall first so a concurrent reader sees a consistent pair:
        // it may use a slightly stale tsc_base, but that just adds a tiny
        // offset (<< 1µs) rather than a sign error.
        wall_base_ns_.store(c.wall_ns, std::memory_order_release);
        tsc_base_.store(c.tsc, std::memory_order_release);
    }

   private:
    struct Correlation {
        uint64_t tsc;
        int64_t wall_ns;
    };

    // Snap a simultaneous (tsc, realtime_ns) pair.
    // Calls clock_gettime twice around RDTSCP to minimise skew; takes midpoint.
    static Correlation snap_correlation() noexcept {
        struct timespec t0 {
        }, t1{};
        clock_gettime(CLOCK_REALTIME, &t0);
        uint64_t tsc = rdtscp();
        clock_gettime(CLOCK_REALTIME, &t1);

        // Midpoint wall time to minimise asymmetric syscall latency.
        int64_t w0 = int64_t(t0.tv_sec) * 1'000'000'000LL + t0.tv_nsec;
        int64_t w1 = int64_t(t1.tv_sec) * 1'000'000'000LL + t1.tv_nsec;
        return {tsc, (w0 + w1) / 2};
    }

    // Calibrate TSC frequency against CLOCK_MONOTONIC_RAW over ~50 ms.
    // MONOTONIC_RAW shares the same oscillator as TSC and is unaffected by NTP.
    static double calibrate_ghz() {
        rdtscp();  // warm up

        struct timespec t0 {
        }, t1{};
        clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
        uint64_t tsc0 = rdtscp();

        // Busy-spin for 50 ms.
        struct timespec target = t0;
        target.tv_nsec += 50'000'000;
        if (target.tv_nsec >= 1'000'000'000) {
            target.tv_nsec -= 1'000'000'000;
            ++target.tv_sec;
        }
        do {
            clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
        } while (t1.tv_sec < target.tv_sec ||
                 (t1.tv_sec == target.tv_sec && t1.tv_nsec < target.tv_nsec));

        uint64_t tsc1 = rdtscp();

        uint64_t elapsed_ns = uint64_t(t1.tv_sec - t0.tv_sec) * 1'000'000'000ULL +
                              uint64_t(t1.tv_nsec) - uint64_t(t0.tv_nsec);
        return double(tsc1 - tsc0) / double(elapsed_ns);
    }

    double ghz_;
    std::atomic<uint64_t> tsc_base_{0};
    std::atomic<int64_t> wall_base_ns_{0};
};

}  // namespace tsc
