#pragma once

#include "fluxnet/message.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fluxnet::detail {

class BufferPool;

// Move-only block. Returns memory to its pool (or deletes it) on destruction.
// Not thread-safe. The pool that issued it must be used from the same worker.
class PooledBuffer {
  public:
    PooledBuffer() = default;
    ~PooledBuffer() { reset(); }

    PooledBuffer(const PooledBuffer&) = delete;
    PooledBuffer& operator=(const PooledBuffer&) = delete;

    PooledBuffer(PooledBuffer&& other) noexcept { move_from(other); }
    PooledBuffer& operator=(PooledBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    std::byte* data() noexcept { return data_; }
    const std::byte* data() const noexcept { return data_; }
    std::size_t capacity() const noexcept { return cap_; }
    std::size_t size() const noexcept { return size_; }
    void set_size(std::size_t n) noexcept { size_ = n; }
    explicit operator bool() const noexcept { return data_ != nullptr; }

  private:
    friend class BufferPool;
    void reset() noexcept;
    void move_from(PooledBuffer& other) noexcept {
        data_ = other.data_;
        cap_ = other.cap_;
        size_ = other.size_;
        pool_ = other.pool_;
        klass_ = other.klass_;
        other.data_ = nullptr;
        other.cap_ = 0;
        other.size_ = 0;
        other.pool_ = nullptr;
        other.klass_ = -1;
    }

    std::byte* data_ = nullptr;
    std::size_t cap_ = 0;
    std::size_t size_ = 0;
    BufferPool* pool_ = nullptr;
    int klass_ = -1;
};

struct PoolStats {
    std::uint64_t acquires = 0;
    std::uint64_t reuses = 0;
    std::uint64_t allocs = 0;
    std::uint64_t bytes_in_use = 0;
    std::uint64_t bytes_high_watermark = 0;
};

// Worker-local. Size classes: 1 KiB, 4 KiB, 16 KiB, 64 KiB.
// Blocks larger than 64 KiB are allocated directly and never retained.
// Freelist length per class is capped. Retained blocks are not leaks.
class BufferPool {
  public:
    BufferPool(std::size_t max_per_class, bool enabled);
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    PooledBuffer acquire(std::size_t n);
    PoolStats stats() const;

  private:
    friend class PooledBuffer;
    void release(std::byte* data, int klass, std::size_t cap) noexcept;

    static constexpr std::size_t kClasses[4] = {1024, 4096, 16384, 65536};

    bool enabled_ = true;
    std::size_t limit_ = 0;
    std::vector<std::byte*> free_[4];
    std::atomic<std::uint64_t> acquires_{0};
    std::atomic<std::uint64_t> reuses_{0};
    std::atomic<std::uint64_t> allocs_{0};
    std::atomic<std::uint64_t> bytes_in_use_{0};
    std::atomic<std::uint64_t> bytes_high_{0};
};

PooledBuffer encode_frame(BufferPool& pool, MessageType type, std::uint16_t flags,
                          std::span<const std::byte> payload, std::size_t max_frame);

} // namespace fluxnet::detail
