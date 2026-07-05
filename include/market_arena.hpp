#pragma once
#include <memory>
#include <memory_resource>
#include <vector>
#include <stdexcept>
#include <string>
#include <cstddef>

namespace exchange {

// Per-market memory arena built on C++17 pmr (polymorphic memory resource).
//
// All OrderBook containers (bids map, asks map, order id map, price-level deques)
// draw memory from this arena. No allocations go to the system heap during matching.
//
// Allocation stack (bottom to top):
//   buffer_    — fixed backing store, allocated once at construction. Never grows.
//   monotonic_ — bump allocator over buffer_; upstream is null_memory_resource()
//                so any overflow crashes rather than silently falling back to heap.
//   tracking_  — thin wrapper that tracks total bytes committed to the monotonic
//                buffer (counts chunk requests from pool_; individual allocations
//                within a chunk do NOT increment this). Used for the reject watermark.
//   pool_      — unsynchronized_pool_resource: per-size-class free lists. Freed
//                Order/map-node/deque-block memory is recycled here and reused by
//                future allocations without touching the monotonic buffer.
//                Declared LAST in the class so it is destroyed FIRST, before
//                tracking_, ensuring pool_.release() can safely call
//                tracking_->deallocate() during destruction.
//
// Rejection policy:
//   New orders are rejected when committed bytes >= REJECT_WATERMARK (default 99%).
//   "Committed" here means bytes the monotonic buffer has given to the pool in chunks.
//   At steady state (cancel-then-reuse cycles), the pool recycles freed blocks
//   internally without requesting new chunks — committed_ stays flat.
//   The 1% safety reserve (~1MB at 100MB default) guarantees that:
//     (1) the rejection event itself can be allocated and emitted,
//     (2) cancel commands for in-flight orders can still be processed,
//     (3) any internal bookkeeping triggered by those cancels has headroom.
//   Cancel commands are NEVER gated — they only free memory, never increase net usage.
//
// MIN_CAPACITY (16MB) is the hard floor, enforced at construction.

class TrackingResource : public std::pmr::memory_resource {
public:
    explicit TrackingResource(std::pmr::memory_resource* upstream)
        : upstream_(upstream) {}

    // Total bytes committed to the underlying monotonic buffer.
    // Counts the bytes the pool has requested from monotonic in chunks.
    // Does NOT count individual allocations served from the pool's free lists —
    // those are recycled internally and never touch the monotonic again.
    // This accurately reflects physical buffer exhaustion: once chunks are
    // given to the pool they are never returned (monotonic doesn't reclaim).
    // At steady state (cancel-then-reuse cycles) committed_ stays flat.
    size_t committed() const noexcept { return committed_; }

private:
    void* do_allocate(size_t n, size_t align) override {
        void* p = upstream_->allocate(n, align);
        committed_ += n;
        return p;
    }

    void do_deallocate(void* p, size_t n, size_t align) override {
        // Intentional no-op for committed_ — monotonic buffer doesn't reclaim bytes.
        // The dealloc is forwarded upstream so the allocator contract is satisfied,
        // but monotonic_buffer_resource ignores it too.
        upstream_->deallocate(p, n, align);
    }

    bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
        return this == &o;
    }

    std::pmr::memory_resource* upstream_;
    size_t committed_{0};
};

class MarketArena {
public:
    // Hard minimum — below this the book cannot hold a meaningful number of orders.
    static constexpr size_t MIN_CAPACITY = 16ULL * 1024 * 1024;  // 16 MB

    // Default capacity and watermark ratio. Both overridable at construction.
    static constexpr size_t DEFAULT_CAPACITY     = 100ULL * 1024 * 1024;  // 100 MB
    static constexpr double DEFAULT_REJECT_RATIO = 0.99;                  // reject at 99%

    explicit MarketArena(
        size_t capacity    = DEFAULT_CAPACITY,
        double reject_ratio = DEFAULT_REJECT_RATIO)
    {
        if (capacity < MIN_CAPACITY) {
            throw std::invalid_argument(
                "MarketArena capacity must be >= " +
                std::to_string(MIN_CAPACITY / (1024 * 1024)) + " MB, got " +
                std::to_string(capacity / (1024 * 1024)) + " MB");
        }
        if (reject_ratio <= 0.0 || reject_ratio >= 1.0) {
            throw std::invalid_argument("reject_ratio must be in (0.0, 1.0)");
        }

        capacity_         = capacity;
        reject_watermark_ = static_cast<size_t>(capacity_ * reject_ratio);

        buffer_.resize(capacity_);

        // null_memory_resource() as upstream: overflow is a hard crash (bad_alloc).
        // We never expect to reach it — the 99% watermark fires first.
        monotonic_ = std::make_unique<std::pmr::monotonic_buffer_resource>(
            buffer_.data(), buffer_.size(), std::pmr::null_memory_resource());

        // tracking_ wraps monotonic_. It increments committed_ only when the pool
        // requests new chunks from monotonic. Individual container allocations that
        // are served from the pool's free lists do not touch tracking_.
        tracking_ = std::make_unique<TrackingResource>(monotonic_.get());

        // pool_ sits on top of tracking_. It maintains per-size-class free lists.
        // Freed Order/map-node/deque-block memory is recycled here without consuming
        // additional monotonic buffer space. Declared LAST (destroyed FIRST).
        //
        // Pool options: cap chunk sizes so pool never requests more than ~32KB per
        // chunk from tracking_. This keeps the gap between the 99% watermark and
        // the physical buffer limit (1% safety reserve ≈ 160KB for a 16MB arena)
        // safely above the largest possible pool chunk request.
        std::pmr::pool_options pool_opts;
        pool_opts.max_blocks_per_chunk       = 32;   // max 32 blocks per chunk
        pool_opts.largest_required_pool_block = 1024; // blocks > 1KB go direct to upstream
        pool_ = std::make_unique<std::pmr::unsynchronized_pool_resource>(pool_opts, tracking_.get());
    }

    // Non-copyable, movable.
    MarketArena(const MarketArena&)            = delete;
    MarketArena& operator=(const MarketArena&) = delete;
    MarketArena(MarketArena&&)                 = default;
    MarketArena& operator=(MarketArena&&)      = default;

    std::pmr::memory_resource* resource() noexcept { return pool_.get(); }

    // Returns true when new Place orders should be rejected.
    // Cancels are always allowed regardless of this flag.
    bool should_reject_new_orders() const noexcept {
        return tracking_->committed() >= reject_watermark_;
    }

    size_t capacity()         const noexcept { return capacity_; }
    size_t used_bytes()       const noexcept { return tracking_->committed(); }
    size_t free_bytes()       const noexcept { return capacity_ - tracking_->committed(); }
    size_t reject_watermark() const noexcept { return reject_watermark_; }

private:
    size_t capacity_{0};
    size_t reject_watermark_{0};

    std::vector<std::byte>                                   buffer_;
    std::unique_ptr<std::pmr::monotonic_buffer_resource>     monotonic_;
    std::unique_ptr<TrackingResource>                        tracking_;
    // pool_ is declared LAST so it is destroyed FIRST (reverse member order).
    // Its destructor calls release() which deallocates chunks back to tracking_.
    // tracking_ is still alive at that point — no use-after-free.
    std::unique_ptr<std::pmr::unsynchronized_pool_resource>  pool_;
};

} // namespace exchange
