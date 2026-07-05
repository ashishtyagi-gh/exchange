#pragma once
#include <thread>
#include <chrono>

namespace exchange {

// Consumer idle policies — injected into Consumer<> via template parameter.
//
// SpinPolicy:    Never yields or sleeps. Lowest possible wake latency.
//                Use in production on a dedicated core (NUMA-pinned, CPU-isolated
//                via isolcpus kernel param). Intentionally burns 100% of the core.
//
// BackoffPolicy: Spin → yield → sleep ladder. CPU-friendly for dev/test on shared
//                hardware. Adds up to ~50µs wake latency in the worst case (sustained
//                idle), which is acceptable outside the critical path.
//                Always used in tests to avoid burning CI machines.
//
// The idle counter is per Consumer thread, not per market. It resets to 0 whenever
// any market on that thread produces work during a sweep. This means the thread only
// backs off when ALL its markets are simultaneously idle — one active market keeps
// the thread at full spin speed regardless of the others.

struct SpinPolicy {
    // No-op: spin unconditionally.
    // Assumes this thread owns a dedicated, isolated core.
    void on_idle(int /*idle_count*/) const noexcept {}
    void reset() const noexcept {}
};

struct BackoffPolicy {
    // Thresholds are tunable. Current values give:
    //   0–99 iterations  : pure spin   (~nanoseconds)
    //   100–999 iterations: sched_yield (~microseconds, OS-dependent)
    //   1000+ iterations  : 50µs sleep  (acceptable for sustained idle on shared hw)
    static constexpr int SPIN_ITERS  = 100;
    static constexpr int YIELD_ITERS = 1000;

    void on_idle(int idle_count) const noexcept {
        if (idle_count < SPIN_ITERS) {
            // spin — do nothing, stay hot
        } else if (idle_count < YIELD_ITERS) {
            std::this_thread::yield();  // give up timeslice, let other threads run
        } else {
            // Park the thread. Adds ~50–100µs wake latency.
            // Acceptable only during sustained idle; resets immediately on any work.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    void reset() const noexcept {}
};

} // namespace exchange
