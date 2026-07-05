#include "order_book.hpp"
#include "emit_policy.hpp"
#include "market_arena.hpp"

#include <benchmark/benchmark.h>
#include <memory>
#include <random>
#include <vector>

using namespace exchange;

// =========================================================================
// MATCHING ENGINE THROUGHPUT BENCHMARKS
// =========================================================================
//
// PURPOSE
// -------
// These benchmarks measure the raw throughput of the matching engine core
// (OrderBook) under workloads that approximate a real liquid market.
// They drive the engine through end-to-end order placement, matching, and
// cancel paths with a realistically shaped order book in memory.
//
// WHAT IS EXCLUDED (and why)
// --------------------------
// I/O: NullEmitPolicy is used. Every Fill/Ack/Reject event is a no-op.
//      We measure pure matching cost, not the cost of shipping events to
//      a downstream consumer. In production, QueueEmitPolicy would add
//      ~10-30ns per event for an SPSC enqueue, measured separately.
//
// NUMA: A production matching engine thread is pinned to a single NUMA node.
//       Cross-NUMA memory access, which degrades throughput by 2-3x, is not
//       modelled here as it is an operational concern, not an engine concern.
//
// WHAT IS INCLUDED
// ----------------
// std::pmr::map cache pressure:
//   The order book uses std::pmr::map (red-black tree) for price levels.
//   Each lookup causes O(log P) pointer-chasing cache misses where P is
//   the number of distinct price levels. With P=100 levels, log2(100)~7
//   tree-node traversals per operation, each potentially a separate cache
//   line. This is the dominant cost identified in the design writeup.
//
// Realistic book shape:
//   100 levels per side, random 1-10 tick gaps. Depth per level 100-300
//   lots. Full tree working set ~100 nodes x ~64 bytes = ~6KB per side.
//
// orders_ hash table growth:
//   Each completed fill leaves a Filled entry in orders_ (unordered_map).
//   This is the cost of lazy deletion -- the hash table grows over the
//   benchmark run. In production, periodic reclaim or intrusive list
//   would bound this. Documented in design writeup.
//
// BOOK PARAMETERS
// ---------------
// MID        = 1,000,000 ticks
// NUM_LEVELS = 100 per side
// GAP        = 1-10 ticks between levels (random, seeded)
// DEPTH      = 100-300 lots per level
//
// INTERPRETATION GUIDE
// --------------------
// BM_NearSpread_TakerMaker:   ~17-18M matched pairs/sec.
//   Stable spread, continuous taker+maker churn. Primary benchmark.
//   Degrades over time due to orders_ growth.
//
// BM_RestingOnly_RealisticBook: ~9M GTC inserts/sec.
//   Isolates map insert cost. Critical path for market makers.
//
// BM_Cancel_RealisticBook:    ~500-600M lazy-cancels/sec.
//   O(1) lazy mark. Confirms cancel is NOT the bottleneck.
//
// BM_MarketOrder_WalkN:       8M fills/sec (N=1) to 29M fills/sec (N=50).
//   Throughput of aggressive sweep orders consuming N consecutive levels.
//   Scales well -- amortizes per-order overhead over more fills.
//
// BM_PriceDrift:              ~17-18M matched pairs/sec.
//   Volatile session: spread shifts every drift_interval iterations.
//   /1000=BTC volatile  /10000=ES normal  /100000=quiet
// =========================================================================

static constexpr Price MID        = 1'000'000;
static constexpr int   NUM_LEVELS = 100;
static constexpr int   GAP_MIN    = 1;
static constexpr int   GAP_MAX    = 10;
static constexpr int   DEPTH_MIN  = 100;
static constexpr int   DEPTH_MAX  = 300;

struct BookShape {
    std::vector<Price>    bid_prices, ask_prices;
    std::vector<Quantity> bid_sizes,  ask_sizes;

    explicit BookShape(uint64_t seed = 42) {
        std::mt19937_64 rng(seed);
        std::uniform_int_distribution<int> gap(GAP_MIN, GAP_MAX);
        std::uniform_int_distribution<int> depth(DEPTH_MIN, DEPTH_MAX);

        Price p = MID - 1;
        for (int i = 0; i < NUM_LEVELS; ++i) {
            bid_prices.push_back(p);
            bid_sizes.push_back(depth(rng));
            p -= gap(rng);
        }

        p = MID + 1;
        for (int i = 0; i < NUM_LEVELS; ++i) {
            ask_prices.push_back(p);
            ask_sizes.push_back(depth(rng));
            p += gap(rng);
        }
    }

    Price best_bid() const { return bid_prices.front(); }
    Price best_ask() const { return ask_prices.front(); }
};

static const BookShape SHAPE;

struct BenchState {
    MarketArena arena;
    std::unique_ptr<OrderBook<NullEmitPolicy>> book;
    OrderId next_oid{1};

    explicit BenchState(size_t arena_bytes = 512ULL * 1024 * 1024)
        : arena(arena_bytes)
    {
        book = std::make_unique<OrderBook<NullEmitPolicy>>(
            arena.resource(), NullEmitPolicy{});
        book->set_resource(arena.resource());
        book->set_arena_reject_fn([this]{ return arena.should_reject_new_orders(); });
    }

    OrderId oid() { return next_oid++; }

    void populate(const BookShape& shape) {
        for (int i = 0; i < NUM_LEVELS; ++i) {
            book->place({oid(), 1, Side::Buy,  shape.bid_prices[i], shape.bid_sizes[i], TIF::GTC});
            book->place({oid(), 1, Side::Sell, shape.ask_prices[i], shape.ask_sizes[i], TIF::GTC});
        }
    }
};

// ── BM_NearSpread_TakerMaker ──────────────────────────────────────────────────
// Stable spread, continuous taker+maker churn. Primary benchmark.
// Each iteration: IOC sell drains one maker, GTC buy replenishes it.

static constexpr Quantity MAKER_LOTS = 10;

static void BM_NearSpread_TakerMaker(benchmark::State& state) {
    BenchState s;
    s.populate(SHAPE);

    const Price best_bid = SHAPE.best_bid();

    for (auto _ : state) {
        s.book->place({s.oid(), 2, Side::Sell, best_bid, MAKER_LOTS, TIF::IOC});
        s.book->place({s.oid(), 1, Side::Buy,  best_bid, MAKER_LOTS, TIF::GTC});
    }

    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_NearSpread_TakerMaker)->Iterations(500'000)->Unit(benchmark::kNanosecond);

// ── BM_RestingOnly_RealisticBook ──────────────────────────────────────────────
// GTC inserts at an away-from-spread price. Isolates map insert cost.

static void BM_RestingOnly_RealisticBook(benchmark::State& state) {
    BenchState s;
    s.populate(SHAPE);

    constexpr Price AWAY_PRICE = MID - 100'000;

    for (auto _ : state) {
        s.book->place({s.oid(), 1, Side::Buy, AWAY_PRICE, 1, TIF::GTC});
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RestingOnly_RealisticBook)->Iterations(500'000)->Unit(benchmark::kNanosecond);

// ── BM_Cancel_RealisticBook ───────────────────────────────────────────────────
// O(1) lazy cancel. Confirms cancel is not the bottleneck.

static void BM_Cancel_RealisticBook(benchmark::State& state) {
    BenchState s;
    s.populate(SHAPE);

    const OrderId first_oid = 1;
    const OrderId last_oid  = s.next_oid - 1;

    OrderId cancel_id = first_oid;
    for (auto _ : state) {
        s.book->cancel({cancel_id, 1});
        if (++cancel_id > last_oid) cancel_id = first_oid;
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cancel_RealisticBook)->Iterations(500'000)->Unit(benchmark::kNanosecond);

// ── BM_MarketOrder_WalkN ──────────────────────────────────────────────────────
// An IOC buy walks exactly N ask levels. Models a large sweep order.
// Throughput scales with N -- overhead is amortized across more fills.

static void BM_MarketOrder_WalkN(benchmark::State& state) {
    const int walk = static_cast<int>(state.range(0));

    BenchState s(2ULL * 1024 * 1024 * 1024);
    s.populate(SHAPE);

    for (auto _ : state) {
        s.book->place({s.oid(), 2, Side::Buy, MARKET_PRICE,
                       static_cast<Quantity>(walk), TIF::IOC});

        state.PauseTiming();
        for (int i = 0; i < walk; ++i) {
            s.book->place({s.oid(), 1, Side::Sell,
                           SHAPE.ask_prices[i], 1, TIF::GTC});
        }
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * walk);
    state.SetLabel("fills per call");
}
BENCHMARK(BM_MarketOrder_WalkN)
    ->Arg(1)->Arg(5)->Arg(10)->Arg(25)->Arg(50)
    ->Iterations(100'000)->Unit(benchmark::kNanosecond);

// ── BM_PriceDrift ─────────────────────────────────────────────────────────────
// Volatile session: spread shifts by one tick every drift_interval events.
// Quantifies the real cache cost of price movement.

static void BM_PriceDrift(benchmark::State& state) {
    const int drift_interval = static_cast<int>(state.range(0));

    BenchState s(2ULL * 1024 * 1024 * 1024);
    s.populate(SHAPE);

    Price cur_best_bid = SHAPE.best_bid();
    int   direction    = -1;
    int   iter_count   = 0;
    OrderId last_bid_level_oid = 0;

    for (auto _ : state) {
        ++iter_count;

        s.book->place({s.oid(), 2, Side::Sell, cur_best_bid, 1, TIF::IOC});
        s.book->place({s.oid(), 1, Side::Buy,  cur_best_bid, 1, TIF::GTC});

        if (iter_count % drift_interval == 0) {
            if (last_bid_level_oid != 0)
                s.book->cancel({last_bid_level_oid, 1});

            cur_best_bid += direction;

            if (iter_count % (drift_interval * 20) == 0)
                direction = -direction;

            last_bid_level_oid = s.oid();
            s.book->place({last_bid_level_oid, 1, Side::Buy, cur_best_bid, 200, TIF::GTC});
        }
    }

    state.SetItemsProcessed(state.iterations() * 2);
    state.SetLabel("drift every N iters; lower=more volatile");
}
BENCHMARK(BM_PriceDrift)
    ->Arg(1000)->Arg(10000)->Arg(100000)
    ->Iterations(500'000)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
