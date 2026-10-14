// Lock-free single-producer / single-consumer ring buffer of floats.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace road {

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 0) { resize(capacity); }
    void resize(size_t capacity) {
        size_t sz = 1;
        while (sz < capacity + 1) sz <<= 1;
        buf_.assign(sz, 0.0f);
        mask_ = sz - 1;
        r_.store(0); w_.store(0);
    }
    size_t capacity() const { return buf_.empty() ? 0 : buf_.size() - 1; }
    size_t readable() const { return (w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed)) & mask_; }
    size_t writable() const { return capacity() - readable(); }

    size_t write(const float* src, size_t n) {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t r = r_.load(std::memory_order_acquire);
        const size_t free = capacity() - ((w - r) & mask_);
        n = std::min(n, free);
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = src[i];
        w_.store((w + n) & mask_, std::memory_order_release);
        return n;
    }
    size_t read(float* dst, size_t n) {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, (w - r) & mask_);
        for (size_t i = 0; i < n; ++i) dst[i] = buf_[(r + i) & mask_];
        r_.store((r + n) & mask_, std::memory_order_release);
        return n;
    }
    size_t skip(size_t n) {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, (w - r) & mask_);
        r_.store((r + n) & mask_, std::memory_order_release);
        return n;
    }
    // peek at element i (0 = oldest readable) without consuming
    float peek(size_t i) const { return buf_[(r_.load(std::memory_order_relaxed) + i) & mask_]; }
    void clear() { r_.store(w_.load()); }

private:
    std::vector<float> buf_;
    size_t mask_ = 0;
    std::atomic<size_t> r_{0}, w_{0};
};

} // namespace road
