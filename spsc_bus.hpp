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
// Usage (two separate processes):
//
//   // Process A — create & produce
//   auto bus = spsc::SharedBus::create("/my_bus", /*capacity=*/1024,
//                                      /*max_msg_size=*/256);
//   spsc::Producer prod(bus);
//   prod.push("hello", 5);
//
//   // Process B — open & consume
//   auto bus = spsc::SharedBus::open("/my_bus");
//   spsc::Consumer cons(bus);
//   char buf[256];
//   uint32_t n = cons.pop(buf, sizeof(buf));

#pragma once

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __x86_64__
#include <immintrin.h>
#define SPSC_PAUSE() _mm_pause()
#else
#define SPSC_PAUSE() __asm__ volatile("yield" ::: "memory")
#endif

namespace spsc {

// ── constants ────────────────────────────────────────────────────────────────

inline constexpr size_t kCacheLine = 64;
inline constexpr uint32_t kMagic   = 0x53505343u;  // 'SPSC'

// ── shared-memory layout ─────────────────────────────────────────────────────
//
//  [ ControlBlock  (3 × 64 B) ]
//  [ Slot 0        (stride B) ]
//  [ Slot 1        (stride B) ]
//  ...
//  [ Slot N-1      (stride B) ]
//
//  stride = round_up(sizeof(SlotHeader) + max_msg_size, kCacheLine)

struct SlotHeader {
    uint32_t size;  // payload bytes written into this slot
    uint32_t _pad;
};

struct alignas(kCacheLine) ControlBlock {
    // ── read-only after initialisation ──────────────────────────────────────
    uint32_t magic;
    uint32_t capacity;      // slot count, must be power of 2
    uint32_t slot_stride;   // bytes per slot (SlotHeader + payload, cache-aligned)
    uint32_t max_msg_size;  // maximum payload bytes
    char _pad0[kCacheLine - 4 * sizeof(uint32_t)];

    // ── producer-written, consumer-read ─────────────────────────────────────
    alignas(kCacheLine) std::atomic<uint64_t> write_pos;
    char _pad1[kCacheLine - sizeof(std::atomic<uint64_t>)];

    // ── consumer-written, producer-read ─────────────────────────────────────
    alignas(kCacheLine) std::atomic<uint64_t> read_pos;
    char _pad2[kCacheLine - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(ControlBlock) == 3 * kCacheLine, "ControlBlock must be exactly 3 cache lines");

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
    auto *base   = static_cast<char *>(shm_base) + sizeof(ControlBlock);
    return reinterpret_cast<SlotHeader *>(base + size_t(idx) * stride);
}

// ── SharedBus — owns the mmap'd segment ──────────────────────────────────────

class SharedBus {
   public:
    // Create and initialise a new shared-memory bus (call once, from producer).
    static SharedBus create(const std::string &name, uint32_t capacity, uint32_t max_msg_size) {
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

        void *mem = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (mem == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "mmap(create)");

        // Zero the whole region, then init the control block.
        std::memset(mem, 0, sz);
        auto *cb         = static_cast<ControlBlock *>(mem);
        cb->capacity     = capacity;
        cb->slot_stride  = static_cast<uint32_t>(compute_slot_stride(max_msg_size));
        cb->max_msg_size = max_msg_size;
        cb->write_pos.store(0, std::memory_order_relaxed);
        cb->read_pos.store(0, std::memory_order_relaxed);
        // Write magic last — acts as a visibility fence for the consumer's open().
        std::atomic_thread_fence(std::memory_order_release);
        cb->magic = kMagic;

        return SharedBus{name, mem, sz};
    }

    // Open an existing bus (consumer side).  Spins until the producer has
    // finished initialising (magic becomes valid).
    static SharedBus open(const std::string &name) {
        int fd = shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "shm_open(open)");

        struct stat st {};
        fstat(fd, &st);
        size_t sz = static_cast<size_t>(st.st_size);
        if (sz < sizeof(ControlBlock)) {
            close(fd);
            throw std::runtime_error("shared memory segment too small");
        }

        void *mem = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (mem == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "mmap(open)");

        // Spin until magic is visible (producer may still be writing).
        auto *cb = static_cast<ControlBlock *>(mem);
        while (cb->magic != kMagic)
            SPSC_PAUSE();
        std::atomic_thread_fence(std::memory_order_acquire);

        return SharedBus{name, mem, sz};
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
        auto *cb  = bus_.control();
        capacity_ = cb->capacity;
        stride_   = cb->slot_stride;
        max_msg_  = cb->max_msg_size;
        // Seed local cache from the actual atomic so restarts are safe.
        write_pos_  = cb->write_pos.load(std::memory_order_relaxed);
        read_cache_ = cb->read_pos.load(std::memory_order_acquire);
    }

    // Non-blocking.  Returns true on success, false if the ring is full.
    [[nodiscard]] bool try_push(const void *payload, uint32_t size) noexcept {
        if (size > max_msg_) [[unlikely]]
            return false;

        // Check for space using the cached read position first.
        if (write_pos_ - read_cache_ >= capacity_) {
            // Refresh from memory — consumer may have advanced.
            read_cache_ = bus_.control()->read_pos.load(std::memory_order_acquire);
            if (write_pos_ - read_cache_ >= capacity_)
                return false;
        }

        SlotHeader *slot = slot_ptr(bus_.base(), capacity_, stride_, write_pos_);
        slot->size       = size;
        std::memcpy(slot + 1, payload, size);

        // Release: slot writes must be visible before write_pos advances.
        bus_.control()->write_pos.store(write_pos_ + 1, std::memory_order_release);
        ++write_pos_;
        return true;
    }

    // Blocking spin until space is available.
    void push(const void *payload, uint32_t size) noexcept {
        while (!try_push(payload, size))
            SPSC_PAUSE();
    }

    uint32_t max_msg_size() const noexcept { return max_msg_; }
    uint64_t write_pos() const noexcept { return write_pos_; }

   private:
    SharedBus &bus_;
    uint32_t capacity_;
    uint32_t stride_;
    uint32_t max_msg_;
    uint64_t write_pos_  = 0;
    uint64_t read_cache_ = 0;  // cached consumer position
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
    }

    // Non-blocking.  Returns bytes written into out_buf, or 0 if empty.
    // out_buf must be at least max_msg_size() bytes.
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

        // Release: slot read must complete before read_pos advances.
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

    uint32_t max_msg_size() const noexcept { return max_msg_; }
    uint64_t read_pos() const noexcept { return read_pos_; }

   private:
    SharedBus &bus_;
    uint32_t capacity_;
    uint32_t stride_;
    uint32_t max_msg_;
    uint64_t read_pos_    = 0;
    uint64_t write_cache_ = 0;  // cached producer position
};

}  // namespace spsc
