// spsc_bus.hpp  —  lock-free SPSC message bus over POSIX shared memory
//
// Design choices:
//  • Power-of-2 ring buffer — index wrapping via bitwise AND, no modulo
//  • write_pos / read_pos on separate cache lines — eliminates false sharing
//  • Cached remote position — producer caches read_pos, consumer caches
//    write_pos; cross-core coherence traffic only on actual full/empty checks
//  • Slots are cache-line-stride padded — no inter-slot false sharing
//  • Acquire/release ordering only — no seq_cst, no fences beyond what's needed
//  • _mm_pause() in spin loops — reduces power and pipeline hazards on x86
//
// Phase 2 additions (recovery & fault tolerance):
//  • session_id in ControlBlock — detects stale segments after producer restart
//  • producer_pid / consumer_pid — crash detection via kill(pid, 0)
//  • heartbeat timestamps — liveness signal from each side
//  • SharedBus::open() retry — consumer-first startup with configurable timeout
//  • push_checked / pop_checked — blocking ops that abort on peer death
//
// Phase 3 additions (production hardening):
//  • BusOptions: lock_memory (mlock), huge_pages (MAP_HUGETLB)
//  • OverwriteProducer — lossy mode; overwrites oldest slot when ring is full
//  • Notifier — eventfd-based wakeup for mixed-criticality consumers (Linux)
//
// Usage (two separate processes):
//
//   // Process A — create & produce
//   auto bus = spsc::SharedBus::create("/my_bus", /*capacity=*/1024,
//                                      /*max_msg_size=*/256);
//   spsc::Producer prod(bus);
//   prod.push("hello", 5);
//
//   // Process B — open & consume (retries until producer creates segment)
//   auto bus = spsc::SharedBus::open("/my_bus", /*timeout_ms=*/5000);
//   spsc::Consumer cons(bus);
//   char buf[256];
//   uint32_t n = cons.pop(buf, sizeof(buf));

#pragma once

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <poll.h>
#include <sys/eventfd.h>
#define SPSC_HAS_EVENTFD 1
#endif

#ifdef __x86_64__
#include <immintrin.h>
#define SPSC_PAUSE() _mm_pause()
#else
#define SPSC_PAUSE() __asm__ volatile("yield" ::: "memory")
#endif

namespace spsc {

// ── constants ────────────────────────────────────────────────────────────────

inline constexpr size_t kCacheLine      = 64;
inline constexpr uint32_t kMagic        = 0x53505343u;  // 'SPSC'
inline constexpr int kDefaultCheckEvery = 1024;         // spins between peer-liveness checks

// ── peer liveness ─────────────────────────────────────────────────────────────

enum class PeerStatus {
    Alive,    // kill(pid, 0) == 0    — peer process is running
    Dead,     // errno == ESRCH       — peer process does not exist
    Unknown,  // pid not yet registered in ControlBlock (0)
};

// ── bus creation options ──────────────────────────────────────────────────────

struct BusOptions {
    bool lock_memory = false;  // mlock() — prevent hot-path page faults under memory pressure
    bool huge_pages = false;  // MAP_HUGETLB — reduce TLB pressure (requires aligned sizes ≥ 2 MB)
};

// ── shared-memory layout ─────────────────────────────────────────────────────
//
//  [ ControlBlock  (4 × 64 B) ]
//  [ Slot 0        (stride B) ]
//  [ Slot 1        (stride B) ]
//  ...
//  [ Slot N-1      (stride B) ]
//
//  stride = round_up(sizeof(SlotHeader) + max_msg_size, kCacheLine)

struct SlotHeader {
    uint32_t generation;  // seqlock counter used by OverwriteProducer; 0 in normal use
    uint32_t size;        // payload bytes written into this slot
};

struct alignas(kCacheLine) ControlBlock {
    // ── cache line 0: read-only config after initialisation ───────────────────
    uint32_t magic;
    uint32_t capacity;      // slot count, must be power of 2
    uint32_t slot_stride;   // bytes per slot (SlotHeader + payload, cache-aligned)
    uint32_t max_msg_size;  // maximum payload bytes
    uint64_t session_id;    // random, written at create(); detects stale segments on re-open
    char _pad0[kCacheLine - 4 * sizeof(uint32_t) - sizeof(uint64_t)];

    // ── cache line 1: producer hot path ───────────────────────────────────────
    alignas(kCacheLine) std::atomic<uint64_t> write_pos;
    char _pad1[kCacheLine - sizeof(std::atomic<uint64_t>)];

    // ── cache line 2: consumer hot path ───────────────────────────────────────
    alignas(kCacheLine) std::atomic<uint64_t> read_pos;
    char _pad2[kCacheLine - sizeof(std::atomic<uint64_t>)];

    // ── cache line 3: fault-tolerance metadata (cold path, never on hot path) ─
    alignas(kCacheLine) std::atomic<uint32_t> producer_pid;
    uint32_t _pad3a;
    std::atomic<uint64_t> producer_heartbeat_ts;
    std::atomic<uint32_t> consumer_pid;
    uint32_t _pad3b;
    std::atomic<uint64_t> consumer_heartbeat_ts;
    char _pad3c[kCacheLine - 2 * sizeof(std::atomic<uint32_t>) - 2 * sizeof(uint32_t) -
                2 * sizeof(std::atomic<uint64_t>)];
};

static_assert(sizeof(ControlBlock) == 4 * kCacheLine, "ControlBlock must be exactly 4 cache lines");

// ── size helpers ──────────────────────────────────────────────────────────────

inline size_t compute_slot_stride(uint32_t max_msg_size) noexcept {
    size_t raw = sizeof(SlotHeader) + max_msg_size;
    return (raw + kCacheLine - 1u) & ~(kCacheLine - 1u);
}

inline size_t compute_shm_size(uint32_t capacity, uint32_t max_msg_size) noexcept {
    return sizeof(ControlBlock) + size_t(capacity) * compute_slot_stride(max_msg_size);
}

// ── slot pointer from absolute position ──────────────────────────────────────

inline SlotHeader *slot_ptr(void *shm_base, uint32_t capacity, uint32_t stride,
                            uint64_t pos) noexcept {
    uint32_t idx = static_cast<uint32_t>(pos) & (capacity - 1u);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto *base = static_cast<char *>(shm_base) + sizeof(ControlBlock);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<SlotHeader *>(base + size_t(idx) * stride);
}

// ── seqlock helpers for SlotHeader.generation ────────────────────────────────
// SlotHeader lives in shared memory mapped from a file descriptor. Accessing
// its generation field as std::atomic via reinterpret_cast is technically
// implementation-defined but well-specified on GCC/Clang/x86/ARM in practice.
// This is the standard pattern for lock-free shared memory in C++.

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
inline std::atomic<uint32_t> *gen_ptr(SlotHeader *s) noexcept {
    return reinterpret_cast<std::atomic<uint32_t> *>(&s->generation);
}
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
inline const std::atomic<uint32_t> *gen_ptr(const SlotHeader *s) noexcept {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&s->generation);
}

// ── session ID generation ─────────────────────────────────────────────────────

inline uint64_t make_session_id() noexcept {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
    return t ^ (uint64_t(static_cast<uint32_t>(getpid())) << 32u);
}

// ── monotonic nanoseconds for heartbeat ──────────────────────────────────────

inline uint64_t mono_ns() noexcept {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
}

// ── SharedBus — owns the mmap'd segment ──────────────────────────────────────

class SharedBus {
   public:
    // Create and initialise a new shared-memory bus (call once, from producer).
    static SharedBus create(const std::string &name, uint32_t capacity, uint32_t max_msg_size,
                            BusOptions opts = {}) {
        if (!capacity || (capacity & (capacity - 1)))
            throw std::invalid_argument("capacity must be a power of two");
        if (!max_msg_size)
            throw std::invalid_argument("max_msg_size must be > 0");

        size_t sz = compute_shm_size(capacity, max_msg_size);

        int fd = shm_open(name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "shm_open(create)");

        if (ftruncate(fd, static_cast<off_t>(sz)) < 0) {
            close(fd);
            shm_unlink(name.c_str());
            throw std::system_error(errno, std::generic_category(), "ftruncate");
        }

        int mmap_flags = MAP_SHARED;
#ifdef MAP_HUGETLB
        if (opts.huge_pages)
            mmap_flags |= MAP_HUGETLB;
#endif
        void *mem = mmap(nullptr, sz, PROT_READ | PROT_WRITE, mmap_flags, fd, 0);
        close(fd);
        if (mem == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "mmap(create)");

        if (opts.lock_memory)
            mlock(mem, sz);  // best-effort; fails silently if RLIMIT_MEMLOCK exceeded

        // Zero the whole region, then init the control block.
        std::memset(mem, 0, sz);
        auto *cb         = static_cast<ControlBlock *>(mem);
        cb->capacity     = capacity;
        cb->slot_stride  = static_cast<uint32_t>(compute_slot_stride(max_msg_size));
        cb->max_msg_size = max_msg_size;
        cb->session_id   = make_session_id();
        cb->write_pos.store(0, std::memory_order_relaxed);
        cb->read_pos.store(0, std::memory_order_relaxed);
        cb->producer_pid.store(0, std::memory_order_relaxed);
        cb->producer_heartbeat_ts.store(0, std::memory_order_relaxed);
        cb->consumer_pid.store(0, std::memory_order_relaxed);
        cb->consumer_heartbeat_ts.store(0, std::memory_order_relaxed);
        // Write magic last — acts as a visibility fence for the consumer's open().
        std::atomic_thread_fence(std::memory_order_release);
        cb->magic = kMagic;

        return SharedBus{name, mem, sz};
    }

    // Open an existing bus (consumer side).
    //
    // timeout_ms controls retry behaviour on ENOENT (producer not yet created):
    //   0  — no retry; throws immediately if segment does not exist (legacy behaviour)
    //  -1  — retry indefinitely until the segment appears
    //  >0  — retry for up to timeout_ms milliseconds, then throw
    //
    // Spins on the magic field until the producer finishes initialising.
    static SharedBus open(const std::string &name, int timeout_ms = 0, BusOptions opts = {}) {
        using clock    = std::chrono::steady_clock;
        auto deadline  = clock::now() + std::chrono::milliseconds(timeout_ms);
        bool has_limit = timeout_ms > 0;

        while (true) {
            int fd = shm_open(name.c_str(), O_RDWR, 0);
            if (fd < 0) {
                if (errno == ENOENT && timeout_ms != 0) {
                    if (has_limit && clock::now() >= deadline)
                        throw std::runtime_error("SharedBus::open timed out waiting for producer");
                    struct timespec ts {};
                    ts.tv_nsec = 1'000'000;  // 1 ms
                    nanosleep(&ts, nullptr);
                    continue;
                }
                throw std::system_error(errno, std::generic_category(), "shm_open(open)");
            }

            struct stat st {};
            fstat(fd, &st);
            size_t sz = static_cast<size_t>(st.st_size);
            if (sz < sizeof(ControlBlock)) {
                close(fd);
                // Segment exists but ftruncate not yet done — retry
                if (timeout_ms != 0) {
                    if (has_limit && clock::now() >= deadline)
                        throw std::runtime_error("SharedBus::open timed out (segment too small)");
                    struct timespec ts {};
                    ts.tv_nsec = 1'000'000;
                    nanosleep(&ts, nullptr);
                    continue;
                }
                throw std::runtime_error("shared memory segment too small");
            }

            int mmap_flags = MAP_SHARED;
#ifdef MAP_HUGETLB
            if (opts.huge_pages)
                mmap_flags |= MAP_HUGETLB;
#endif
            void *mem = mmap(nullptr, sz, PROT_READ | PROT_WRITE, mmap_flags, fd, 0);
            close(fd);
            if (mem == MAP_FAILED)
                throw std::system_error(errno, std::generic_category(), "mmap(open)");

            if (opts.lock_memory)
                mlock(mem, sz);

            // Spin until magic is visible (producer may still be writing).
            auto *cb = static_cast<ControlBlock *>(mem);
            while (cb->magic != kMagic)
                SPSC_PAUSE();
            std::atomic_thread_fence(std::memory_order_acquire);

            return SharedBus{name, mem, sz};
        }
    }

    // Remove the shared-memory object from the filesystem.
    static void unlink(const std::string &name) noexcept { shm_unlink(name.c_str()); }

    SharedBus(SharedBus &&o) noexcept : name_(std::move(o.name_)), mem_(o.mem_), size_(o.size_) {
        o.mem_  = nullptr;
        o.size_ = 0;
    }
    SharedBus &operator=(SharedBus &&o) noexcept {
        if (this != &o) {
            if (mem_)
                munmap(mem_, size_);
            name_   = std::move(o.name_);
            mem_    = o.mem_;
            size_   = o.size_;
            o.mem_  = nullptr;
            o.size_ = 0;
        }
        return *this;
    }
    ~SharedBus() {
        if (mem_)
            munmap(mem_, size_);
    }

    SharedBus(const SharedBus &)            = delete;
    SharedBus &operator=(const SharedBus &) = delete;

    ControlBlock *control() const noexcept { return static_cast<ControlBlock *>(mem_); }
    void *base() const noexcept { return mem_; }
    size_t size() const noexcept { return size_; }
    const std::string &name() const noexcept { return name_; }
    // session_id written at create(); changes on every producer restart.
    uint64_t session_id() const noexcept { return control()->session_id; }

   private:
    SharedBus(std::string n, void *m, size_t s) : name_(std::move(n)), mem_(m), size_(s) {}

    std::string name_;
    void *mem_   = nullptr;
    size_t size_ = 0;
};

// ── Producer ──────────────────────────────────────────────────────────────────

class Producer {
   public:
    explicit Producer(SharedBus &bus) noexcept : bus_(bus) {
        auto *cb    = bus_.control();
        capacity_   = cb->capacity;
        stride_     = cb->slot_stride;
        max_msg_    = cb->max_msg_size;
        write_pos_  = cb->write_pos.load(std::memory_order_relaxed);
        read_cache_ = cb->read_pos.load(std::memory_order_acquire);
        // Register this process so the consumer can check liveness.
        cb->producer_pid.store(static_cast<uint32_t>(getpid()), std::memory_order_release);
    }

    // Non-blocking.  Returns true on success, false if ring is full or size > max.
    [[nodiscard]] bool try_push(const void *payload, uint32_t size) noexcept {
        if (size > max_msg_)
            return false;

        if (write_pos_ - read_cache_ >= capacity_) {
            read_cache_ = bus_.control()->read_pos.load(std::memory_order_acquire);
            if (write_pos_ - read_cache_ >= capacity_)
                return false;
        }

        SlotHeader *slot = slot_ptr(bus_.base(), capacity_, stride_, write_pos_);
        slot->size       = size;
        std::memcpy(slot + 1, payload, size);

        bus_.control()->write_pos.store(write_pos_ + 1, std::memory_order_release);
        ++write_pos_;
        return true;
    }

    // Blocking spin until space is available.
    void push(const void *payload, uint32_t size) noexcept {
        while (!try_push(payload, size))
            SPSC_PAUSE();
    }

    // Blocking push with consumer-liveness check.
    // Returns false if the consumer process is confirmed dead (ESRCH).
    // check_every: number of failed push attempts between liveness checks.
    bool push_checked(const void *payload, uint32_t size,
                      int check_every = kDefaultCheckEvery) noexcept {
        int spins = 0;
        while (!try_push(payload, size)) {
            SPSC_PAUSE();
            if (++spins % check_every == 0 && consumer_status() == PeerStatus::Dead)
                return false;
        }
        return true;
    }

    // Update the producer heartbeat timestamp (call periodically from app thread).
    // Allows the consumer to detect a stalled but not-yet-dead producer.
    void heartbeat() noexcept {
        bus_.control()->producer_heartbeat_ts.store(mono_ns(), std::memory_order_release);
    }

    // Check whether the consumer process is still running.
    [[nodiscard]] PeerStatus consumer_status() const noexcept {
        uint32_t pid = bus_.control()->consumer_pid.load(std::memory_order_acquire);
        if (pid == 0)
            return PeerStatus::Unknown;
        if (kill(static_cast<pid_t>(pid), 0) == 0)
            return PeerStatus::Alive;
        return (errno == ESRCH) ? PeerStatus::Dead : PeerStatus::Unknown;
    }

    uint32_t max_msg_size() const noexcept { return max_msg_; }
    uint64_t write_pos() const noexcept { return write_pos_; }

   private:
    SharedBus &bus_;
    uint32_t capacity_;
    uint32_t stride_;
    uint32_t max_msg_;
    uint64_t write_pos_  = 0;
    uint64_t read_cache_ = 0;
};

// ── Consumer ──────────────────────────────────────────────────────────────────

class Consumer {
   public:
    explicit Consumer(SharedBus &bus) noexcept : bus_(bus) {
        auto *cb     = bus_.control();
        capacity_    = cb->capacity;
        stride_      = cb->slot_stride;
        max_msg_     = cb->max_msg_size;
        read_pos_    = cb->read_pos.load(std::memory_order_relaxed);
        write_cache_ = cb->write_pos.load(std::memory_order_acquire);
        // Register this process so the producer can check liveness.
        cb->consumer_pid.store(static_cast<uint32_t>(getpid()), std::memory_order_release);
    }

    // Non-blocking.  Returns bytes written into out_buf, or 0 if ring is empty.
    uint32_t try_pop(void *out_buf, uint32_t buf_size) noexcept {
        if (read_pos_ == write_cache_) {
            write_cache_ = bus_.control()->write_pos.load(std::memory_order_acquire);
            if (read_pos_ == write_cache_)
                return 0;
        }

        SlotHeader *slot = slot_ptr(bus_.base(), capacity_, stride_, read_pos_);
        uint32_t msg_sz  = slot->size;
        uint32_t copy_sz = (msg_sz < buf_size) ? msg_sz : buf_size;
        std::memcpy(out_buf, slot + 1, copy_sz);

        bus_.control()->read_pos.store(read_pos_ + 1, std::memory_order_release);
        ++read_pos_;
        return msg_sz;
    }

    // Non-blocking pop for use with OverwriteProducer (lossy ring).
    // Detects slots overwritten while being read via seqlock on SlotHeader.generation.
    // Sets *overwritten = true if message loss is detected (read_pos fast-forwarded).
    // Returns bytes copied on success, 0 on empty or loss event.
    uint32_t try_pop_lossy(void *out_buf, uint32_t buf_size, bool *overwritten = nullptr) noexcept {
        if (overwritten)
            *overwritten = false;

        if (read_pos_ == write_cache_) {
            write_cache_ = bus_.control()->write_pos.load(std::memory_order_acquire);
            if (read_pos_ == write_cache_)
                return 0;
        }

        // Producer has lapped us — fast-forward to the oldest available slot.
        if (write_cache_ - read_pos_ > capacity_) {
            read_pos_ = write_cache_ - capacity_;
            bus_.control()->read_pos.store(read_pos_, std::memory_order_release);
            if (overwritten)
                *overwritten = true;
            return 0;
        }

        SlotHeader *slot    = slot_ptr(bus_.base(), capacity_, stride_, read_pos_);
        const uint32_t gen1 = gen_ptr(slot)->load(std::memory_order_acquire);

        if (gen1 & 1u) {
            // Slot is currently being written by OverwriteProducer — retry.
            SPSC_PAUSE();
            return 0;
        }

        uint32_t msg_sz  = slot->size;
        uint32_t copy_sz = (msg_sz < buf_size) ? msg_sz : buf_size;
        std::memcpy(out_buf, slot + 1, copy_sz);

        const uint32_t gen2 = gen_ptr(slot)->load(std::memory_order_acquire);
        if (gen2 != gen1) {
            // Slot was overwritten while we were copying — fast-forward.
            if (overwritten)
                *overwritten = true;
            write_cache_ = bus_.control()->write_pos.load(std::memory_order_acquire);
            read_pos_    = (write_cache_ > capacity_) ? write_cache_ - capacity_ : write_cache_;
            bus_.control()->read_pos.store(read_pos_, std::memory_order_release);
            return 0;
        }

        bus_.control()->read_pos.store(read_pos_ + 1, std::memory_order_release);
        ++read_pos_;
        return msg_sz;
    }

    // Blocking spin until a message arrives.
    uint32_t pop(void *out_buf, uint32_t buf_size) noexcept {
        uint32_t n;
        while ((n = try_pop(out_buf, buf_size)) == 0)
            SPSC_PAUSE();
        return n;
    }

    // Blocking pop with producer-liveness check.
    // Returns 0 and sets *peer_dead = true if the producer process has exited.
    uint32_t pop_checked(void *out_buf, uint32_t buf_size, bool *peer_dead = nullptr,
                         int check_every = kDefaultCheckEvery) noexcept {
        if (peer_dead)
            *peer_dead = false;
        int spins = 0;
        uint32_t n;
        while ((n = try_pop(out_buf, buf_size)) == 0) {
            SPSC_PAUSE();
            if (++spins % check_every == 0 && producer_status() == PeerStatus::Dead) {
                if (peer_dead)
                    *peer_dead = true;
                return 0;
            }
        }
        return n;
    }

    // Update the consumer heartbeat timestamp (call periodically from app thread).
    void heartbeat() noexcept {
        bus_.control()->consumer_heartbeat_ts.store(mono_ns(), std::memory_order_release);
    }

    // Check whether the producer process is still running.
    [[nodiscard]] PeerStatus producer_status() const noexcept {
        uint32_t pid = bus_.control()->producer_pid.load(std::memory_order_acquire);
        if (pid == 0)
            return PeerStatus::Unknown;
        if (kill(static_cast<pid_t>(pid), 0) == 0)
            return PeerStatus::Alive;
        return (errno == ESRCH) ? PeerStatus::Dead : PeerStatus::Unknown;
    }

    uint32_t max_msg_size() const noexcept { return max_msg_; }
    uint64_t read_pos() const noexcept { return read_pos_; }

   private:
    SharedBus &bus_;
    uint32_t capacity_;
    uint32_t stride_;
    uint32_t max_msg_;
    uint64_t read_pos_    = 0;
    uint64_t write_cache_ = 0;
};

// ── OverwriteProducer — lossy mode ────────────────────────────────────────────
//
// Always succeeds: if the ring is full it overwrites the oldest slot.
// Uses a seqlock on SlotHeader.generation so the Consumer can detect loss via
// try_pop_lossy(). Normal Consumer::try_pop() may return torn data on overwrite;
// pair this class with Consumer::try_pop_lossy() for correct loss detection.

class OverwriteProducer {
   public:
    explicit OverwriteProducer(SharedBus &bus) noexcept : bus_(bus) {
        auto *cb    = bus_.control();
        capacity_   = cb->capacity;
        stride_     = cb->slot_stride;
        max_msg_    = cb->max_msg_size;
        write_pos_  = cb->write_pos.load(std::memory_order_relaxed);
        read_cache_ = cb->read_pos.load(std::memory_order_acquire);
        cb->producer_pid.store(static_cast<uint32_t>(getpid()), std::memory_order_release);
    }

    // Always pushes.  Returns false only if size > max_msg_size.
    bool push(const void *payload, uint32_t size) noexcept {
        if (size > max_msg_)
            return false;

        SlotHeader *slot = slot_ptr(bus_.base(), capacity_, stride_, write_pos_);

        // Seqlock write: odd generation = writing, even = committed.
        const uint32_t g = gen_ptr(slot)->load(std::memory_order_relaxed);
        gen_ptr(slot)->store(g + 1u, std::memory_order_release);  // mark: writing

        slot->size = size;
        std::memcpy(slot + 1, payload, size);

        // Fence ensures memcpy is visible before generation is committed.
        std::atomic_thread_fence(std::memory_order_release);
        gen_ptr(slot)->store(g + 2u, std::memory_order_relaxed);  // mark: done

        bus_.control()->write_pos.store(write_pos_ + 1, std::memory_order_release);
        ++write_pos_;
        return true;
    }

    void heartbeat() noexcept {
        bus_.control()->producer_heartbeat_ts.store(mono_ns(), std::memory_order_release);
    }

    [[nodiscard]] PeerStatus consumer_status() const noexcept {
        uint32_t pid = bus_.control()->consumer_pid.load(std::memory_order_acquire);
        if (pid == 0)
            return PeerStatus::Unknown;
        if (kill(static_cast<pid_t>(pid), 0) == 0)
            return PeerStatus::Alive;
        return (errno == ESRCH) ? PeerStatus::Dead : PeerStatus::Unknown;
    }

    uint32_t max_msg_size() const noexcept { return max_msg_; }
    uint64_t write_pos() const noexcept { return write_pos_; }

   private:
    SharedBus &bus_;
    uint32_t capacity_;
    uint32_t stride_;
    uint32_t max_msg_;
    uint64_t write_pos_  = 0;
    uint64_t read_cache_ = 0;
};

// ── Notifier — eventfd wakeup for mixed-criticality consumers ─────────────────
//
// Adds one syscall per notify/wait — not for ultra-low-latency hot paths.
// Useful when the consumer should sleep between infrequent message bursts
// rather than burning a dedicated core on a spin loop.
//
// In-process (single process, two threads): use directly.
// Cross-process: the eventfd file descriptor must be passed via a Unix socket
// (SCM_RIGHTS) — not handled here; left for application layer.
//
// Usage:
//   spsc::Notifier notifier;
//   // Producer thread: prod.push(...); notifier.notify();
//   // Consumer thread: notifier.wait(); cons.pop(...);

#ifdef SPSC_HAS_EVENTFD
class Notifier {
   public:
    Notifier() {
        efd_ = eventfd(0, 0);  // blocking mode; read() resets counter to 0
        if (efd_ < 0)
            throw std::system_error(errno, std::generic_category(), "eventfd");
    }

    ~Notifier() {
        if (efd_ >= 0)
            close(efd_);
    }

    Notifier(const Notifier &)            = delete;
    Notifier &operator=(const Notifier &) = delete;

    // Signal: producer calls after push(). Wakes one waiting consumer.
    void notify() noexcept {
        uint64_t v  = 1;
        ssize_t ret = write(efd_, &v, sizeof(v));
        (void)ret;  // counter reaching UINT64_MAX-1 is impossible in practice
    }

    // Wait: blocks until notify() is called (or spurious EINTR).
    // Drain the ring after waking — multiple notifies may collapse into one wake.
    void wait() noexcept {
        uint64_t v;
        ssize_t ret;
        do {
            ret = read(efd_, &v, sizeof(v));
        } while (ret < 0 && errno == EINTR);
    }

    // Wait with timeout.  Returns true if signalled, false on timeout.
    bool wait_for_ms(int ms) noexcept {
        struct pollfd pfd {
            efd_, POLLIN, 0
        };
        int r;
        do {
            r = poll(&pfd, 1, ms);
        } while (r < 0 && errno == EINTR);
        if (r <= 0)
            return false;
        uint64_t v;
        ssize_t ret = read(efd_, &v, sizeof(v));
        (void)ret;
        return true;
    }

    int fd() const noexcept { return efd_; }  // for epoll integration

   private:
    int efd_ = -1;
};
#endif  // SPSC_HAS_EVENTFD

}  // namespace spsc
