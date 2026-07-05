#pragma once
#include <atomic>
#include <array>
#include <cassert>
#include <cstddef>
#include <optional>

namespace exchange {

// Lock-free Single-Producer Single-Consumer ring buffer.
//
// Invariants:
//   - Exactly ONE thread calls push() — the designated producer for this market.
//   - Exactly ONE thread calls try_pop() — the consumer thread that owns this market.
//   - These two constraints are what make this lock-free. Any violation (e.g. two
//     threads calling push()) is undefined behaviour. The mapping is established at
//     startup and is immutable for the lifetime of the queue.
//
// Capacity must be a power of 2. The effective usable capacity is (Capacity - 1)
// because head == tail means empty, so we can never fill all slots.
//
// head_ and tail_ are on separate cache lines to prevent false sharing between
// the producer (writes tail_) and consumer (writes head_).

template <typename T, size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static constexpr size_t MASK = Capacity - 1;

public:
    SpscQueue() : head_(0), tail_(0) {}

    // Producer side. Returns false if the queue is full (back-pressure to caller).
    bool push(const T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        [[assume(tail < Capacity)]];  // C++23: masked index is always in [0, Capacity)
        const size_t next = (tail + 1) & MASK;
        if (next == head_.load(std::memory_order_acquire)) {
            return false;  // full
        }
        buffer_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool push(T&& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        [[assume(tail < Capacity)]];  // C++23: masked index is always in [0, Capacity)
        const size_t next = (tail + 1) & MASK;
        if (next == head_.load(std::memory_order_acquire)) {
            return false;  // full
        }
        buffer_[tail] = std::move(item);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if empty.
    bool try_pop(T& out) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        [[assume(head < Capacity)]];  // C++23: masked index is always in [0, Capacity)
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;  // empty
        }
        out = std::move(buffer_[head]);
        head_.store((head + 1) & MASK, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    size_t size() const noexcept {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (t - h + Capacity) & MASK;
    }

private:
    // Pad each atomic to its own cache line to eliminate false sharing between
    // the producer (writes tail_) and the consumer (writes head_).
    static constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;

    // Buffer lives between the two atomics — both threads only read element
    // slots they "own" (producer owns [tail], consumer owns [head]), so no
    // data race on buffer_ itself given the acquire/release on head_/tail_.
    std::array<T, Capacity> buffer_;
};

} // namespace exchange
