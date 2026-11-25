#include "memory/buffer_pool.hpp"

#include "protocol/codec.hpp"

#include <new>
#include <utility>

namespace fluxnet::detail {

void PooledBuffer::reset() noexcept {
    if (data_ != nullptr) {
        if (pool_ != nullptr) {
            pool_->release(data_, klass_, cap_);
        } else {
            delete[] data_;
        }
    }
    data_ = nullptr;
    cap_ = 0;
    size_ = 0;
    pool_ = nullptr;
    klass_ = -1;
}

BufferPool::BufferPool(std::size_t max_per_class, bool enabled)
    : enabled_(enabled), limit_(max_per_class) {
    if (enabled_) {
        for (auto& list : free_) {
            list.reserve(limit_);
        }
    }
}

BufferPool::~BufferPool() {
    for (auto& list : free_) {
        for (std::byte* p : list) {
            delete[] p;
        }
        list.clear();
    }
}

PooledBuffer BufferPool::acquire(std::size_t n) {
    if (n == 0) {
        n = 1;
    }
    int klass = -1;
    std::size_t cap = n;
    if (enabled_) {
        for (int i = 0; i < 4; ++i) {
            if (n <= kClasses[i]) {
                klass = i;
                cap = kClasses[i];
                break;
            }
        }
    }
    std::byte* block = nullptr;
    if (klass >= 0 && !free_[klass].empty()) {
        block = free_[klass].back();
        free_[klass].pop_back();
        reuses_.fetch_add(1, std::memory_order_relaxed);
    } else {
        block = new std::byte[cap];
        allocs_.fetch_add(1, std::memory_order_relaxed);
    }
    acquires_.fetch_add(1, std::memory_order_relaxed);
    const auto bytes = bytes_in_use_.fetch_add(cap, std::memory_order_relaxed) + cap;
    std::uint64_t prev = bytes_high_.load(std::memory_order_relaxed);
    while (bytes > prev &&
           !bytes_high_.compare_exchange_weak(prev, bytes, std::memory_order_relaxed)) {
    }

    PooledBuffer out;
    out.data_ = block;
    out.cap_ = cap;
    out.size_ = 0;
    out.pool_ = this;
    out.klass_ = klass;
    return out;
}

void BufferPool::release(std::byte* data, int klass, std::size_t cap) noexcept {
    bytes_in_use_.fetch_sub(cap, std::memory_order_relaxed);
    if (data == nullptr) {
        return;
    }
    if (!enabled_ || klass < 0 || free_[static_cast<std::size_t>(klass)].size() >= limit_) {
        delete[] data;
        return;
    }
    free_[static_cast<std::size_t>(klass)].push_back(data);
}

PoolStats BufferPool::stats() const {
    PoolStats s;
    s.acquires = acquires_.load(std::memory_order_relaxed);
    s.reuses = reuses_.load(std::memory_order_relaxed);
    s.allocs = allocs_.load(std::memory_order_relaxed);
    s.bytes_in_use = bytes_in_use_.load(std::memory_order_relaxed);
    s.bytes_high_watermark = bytes_high_.load(std::memory_order_relaxed);
    return s;
}

PooledBuffer encode_frame(BufferPool& pool, MessageType type, std::uint16_t flags,
                          std::span<const std::byte> payload, std::size_t max_frame) {
    if (payload.size() > max_frame || !valid_message_type(static_cast<std::uint16_t>(type))) {
        return {};
    }
    const std::size_t total = 8 + payload.size();
    PooledBuffer buf = pool.acquire(total);
    std::size_t written = 0;
    if (!write_frame(buf.data(), buf.capacity(), type, flags, payload, max_frame, written)) {
        return {};
    }
    buf.set_size(written);
    return buf;
}

} // namespace fluxnet::detail
