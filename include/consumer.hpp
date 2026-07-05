#pragma once
#include "types.hpp"
#include "spsc_queue.hpp"
#include "idle_policy.hpp"
#include "order_book.hpp"
#include <atomic>
#include <thread>
#include <functional>
#include <stdexcept>

namespace exchange {

// Default SPSC queue capacity per market (must be power of 2).
static constexpr size_t DEFAULT_QUEUE_CAPACITY = 4096;

using MarketQueue = SpscQueue<Command, DEFAULT_QUEUE_CAPACITY>;

// ── MarketHandle ─────────────────────────────────────────────────────────────
// Lightweight reference held by Consumer. Points to one market's input queue
// and its type-erased processing function. The process function is a closure
// that calls the concrete OrderBook<EmitPolicy, MapPolicy>::place/cancel —
// this lets Consumer stay independent of EmitPolicy and MapPolicy.

struct MarketHandle {
    MarketQueue*                        queue;
    std::function<void(const Command&)> process;
};

// ── Consumer ─────────────────────────────────────────────────────────────────
//
// Owns exactly ONE market and drives it from a dedicated thread.
//
// Design: one Consumer thread per market (1:1 thread:market mapping).
// Rationale:
//   - Simplest possible dispatch: no round-robin, no fairness concern.
//   - A single market saturates a full core at 30+ M ops/sec (see benchmarks).
//     Sharing a core would cap that.
//   - In production, the thread is pinned to an isolated CPU core and a NIC
//     queue via isolcpus + numactl + IRQ affinity. The 1:1 model makes
//     that pinning explicit and mechanical.
//   - Adding a market = adding one thread + one SPSC queue. Zero contention
//     with existing markets. Throughput scales linearly with available cores.
//
// The SPSC invariant is enforced by construction:
//   - Exactly ONE producer calls queue->push() (gateway/client thread).
//   - Exactly ONE consumer calls queue->try_pop() (this Consumer thread).
//
// Templated on IdlePolicy:
//   SpinPolicy    — burn the core. Use on a dedicated, isolated CPU in production.
//   BackoffPolicy — spin→yield→sleep. Use in tests/dev to be CI-friendly.

template <typename IdlePolicy>
class Consumer {
public:
    explicit Consumer(MarketHandle market) : market_(std::move(market)) {}

    // Non-copyable, non-movable (owns a running thread).
    Consumer(const Consumer&)            = delete;
    Consumer& operator=(const Consumer&) = delete;

    void start() {
        if (!market_.queue) throw std::logic_error("Consumer: MarketHandle has null queue");
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&Consumer::run, this);
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    ~Consumer() { stop(); }

private:
    void run() {
        IdlePolicy policy;
        int idle = 0;

        while (running_.load(std::memory_order_acquire)) {
            Command cmd;
            if (market_.queue->try_pop(cmd)) {
                market_.process(cmd);
                idle = 0;               // got work — reset backoff
            } else {
                policy.on_idle(++idle); // empty — apply idle tier
            }
        }

        // Drain any commands enqueued between the last try_pop and stop().
        // Guarantees no events are lost on clean shutdown.
        Command cmd;
        while (market_.queue->try_pop(cmd)) {
            market_.process(cmd);
        }
    }

    MarketHandle      market_;
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace exchange
