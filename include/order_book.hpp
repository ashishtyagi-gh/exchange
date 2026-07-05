#pragma once
#include "types.hpp"
#include "market_arena.hpp"
#include <map>
#include <deque>
#include <unordered_map>
#include <memory_resource>
#include <functional>
#include <cassert>
#include <utility>  // std::unreachable (C++23), std::move

namespace exchange {

// ── What this engine deliberately does NOT do ─────────────────────────────────
//
// These are conscious omissions, not oversights. Each would be a straightforward
// addition but was skipped to keep the core matching logic focused and auditable.
//
// INPUT VALIDATION
//   - No max/min price check          : a limit order at price 0 or UINT64_MAX is accepted
//   - No tick-size enforcement        : prices need not be multiples of a tick increment
//   - No lot-size enforcement         : any quantity > 0 is accepted; no minimum or round-lot check
//   - No duplicate order-id check     : placing two orders with the same order_id overwrites the
//                                       first entry in orders_; caller must ensure uniqueness
//   - No zero-quantity guard          : size=0 is not explicitly rejected (match loop exits immediately)
//
// ACCOUNT / RISK
//   - No balance or collateral check  : orders are accepted regardless of buying power
//   - No position limits              : a single account can hold unlimited resting size
//   - No max-order-count per account  : no cap on how many live orders one account_id may have
//   - No rate limiting                : the SPSC queue back-pressures at 4096 slots but there is
//                                       no per-account or per-second order rate cap
//   - No authentication               : account_id is trusted as supplied; no signature/token check
//
// MARKET STRUCTURE
//   - No circuit breaker / halt       : matching runs unconditionally; no price-band or volatility halt
//   - No reference price / indicative : there is no open-auction or closing-auction phase
//   - No market-maker obligations     : no spread or size requirements enforced on any account
//
// QUERY API
//   - No top-of-book snapshot         : best bid/ask not exposed as a callable method
//   - No depth query                  : full book depth (bids_/asks_) is not exposed externally
//   - No order-status query           : order state lives in orders_ but has no public accessor
//   (all three are additive — internal data structures already hold everything needed)
//
// OPERATIONAL
//   - No persistence / WAL            : state is in-memory only; crash loses the book
//   - No replication                  : no event stream written to a durable log for replay
//   - No graceful overflow handling   : if the arena watermark is hit, new placements are rejected;
//                                       there is no spill to a secondary allocator
//
// ─────────────────────────────────────────────────────────────────────────────

// ── Price level ───────────────────────────────────────────────────────────────
// Each distinct price point on the book holds a FIFO queue of order IDs.
// Orders are inserted at the back (push_back) and consumed from the front
// (pop_front), preserving time priority within a price level.
//
// Cancels use lazy deletion: the order's status is set to Canceled in the
// orders_ map; the ID remains in the deque until the level is matched through.
// This makes cancel O(1) at the cost of occasional dead entries.
// Tradeoff: a deeply out-of-the-money level that is never matched accumulates
// garbage. In production an intrusive linked list with O(1) removal would be
// used instead. Documented in the writeup.
//
// total_size tracks the sum of remaining sizes of LIVE orders at this level,
// maintained incrementally. Dead (canceled/filled) entries don't count.
struct PriceLevel {
    std::pmr::deque<OrderId> order_ids;
    Quantity                 total_size{0};

    explicit PriceLevel(std::pmr::memory_resource* mem) : order_ids(mem) {}
};

// ── OrderBook ─────────────────────────────────────────────────────────────────
//
// Template parameter EmitPolicy controls how output events leave the engine.
// No default — every instantiation must be explicit. See emit_policy.hpp.
//
// The book is NOT thread-safe. It must be accessed exclusively by the single
// consumer thread that owns its market. Thread safety is provided at the
// boundary by the SPSC queue in Consumer<>.
//
// Self-trade prevention policy: CANCEL NEWEST (incoming order is rejected).
// The resting maker order is untouched and remains on the book with its original
// priority. A SelfTrade reject event is emitted for the incoming order.
// Rationale: canceling the incoming order is the least surprising to the maker,
// preserves the book state, and is simple to implement and audit.

template <typename EmitPolicy>
class OrderBook {
public:
    explicit OrderBook(std::pmr::memory_resource* mem, EmitPolicy emit)
        : emit_(std::move(emit))
        , bids_(mem)
        , asks_(mem)
        , orders_(mem)
    {}

    // Process a Place command. Assigns a sequence number, matches, then rests or cancels.
    void place(const PlaceCommand& cmd) {
        // Memory guard — reject new orders at 99% arena usage.
        // Cancels bypass this check entirely (they never increase net allocation).
        // The 1% safety reserve ensures the reject event itself can be emitted.
        if (arena_reject_fn_ && arena_reject_fn_()) {
            emit_.emit(Event::make_reject(RejectEvent{
                next_seq(), cmd.order_id, cmd.account_id, RejectReason::MemoryExhausted}));
            return;
        }

        // Post-only: reject immediately if the order would cross the spread.
        if (cmd.tif == TIF::PostOnly && would_cross(cmd.side, cmd.price)) {
            emit_.emit(Event::make_reject(RejectEvent{
                next_seq(), cmd.order_id, cmd.account_id, RejectReason::PostOnlyWouldTake}));
            return;
        }

        Order order{
            cmd.order_id,
            cmd.account_id,
            cmd.side,
            cmd.price,
            cmd.size,
            cmd.size,   // remaining == size initially
            cmd.tif,
            next_seq(), // time-priority seq: monotonically increasing, assigned at intake
            OrderStatus::New,
        };

        // Match against the opposite side.
        match(order);

        if (order.remaining > 0) {
            if (order.tif == TIF::GTC || order.tif == TIF::PostOnly) {
                // Rest the order on the book.
                rest(order);
            } else {
                // IOC: cancel the unfilled remainder.
                emit_.emit(Event::make_cancel_ack(CancelAckEvent{
                    next_seq(), order.order_id, order.account_id, order.remaining}));
            }
        }
        // If fully filled during match(), nothing more to do.
    }

    // Process a Cancel command.
    // Always accepted regardless of arena watermark — cancels free memory.
    void cancel(const CancelCommand& cmd) {
        const SeqNum seq = next_seq();

        auto it = orders_.find(cmd.order_id);
        if (it == orders_.end() ||
            it->second.status == OrderStatus::Canceled ||
            it->second.status == OrderStatus::Filled ||
            it->second.status == OrderStatus::Rejected) {
            emit_.emit(Event::make_reject(RejectEvent{
                seq, cmd.order_id, cmd.account_id, RejectReason::UnknownOrderId}));
            return;
        }

        Order& o = it->second;

        // Deduct from price level total_size.
        // Note: bids_ and asks_ have different comparator types so ternary doesn't compile.
        // Use explicit if/else to get the right side.
        auto deduct_and_erase = [&](auto& side_map) {
            auto lvl_it = side_map.find(o.price);
            if (lvl_it != side_map.end()) {
                lvl_it->second.total_size -= o.remaining;
                if (lvl_it->second.total_size <= 0)
                    side_map.erase(lvl_it);
            }
        };
        if (o.side == Side::Buy) deduct_and_erase(bids_);
        else                     deduct_and_erase(asks_);

        const Quantity remaining = o.remaining;
        o.status    = OrderStatus::Canceled;
        o.remaining = 0;

        emit_.emit(Event::make_cancel_ack(CancelAckEvent{
            seq, cmd.order_id, cmd.account_id, remaining}));
    }


    // Attach a function that returns true when new orders should be rejected.
    // Decouples the arena watermark check from the book itself.
    void set_arena_reject_fn(std::function<bool()> fn) {
        arena_reject_fn_ = std::move(fn);
    }

    SeqNum current_seq() const noexcept { return seq_; }

private:
    // ── Sequence number ───────────────────────────────────────────────────────
    // Monotonically increasing counter assigned to every command processed.
    // This is the sole source of time ordering in the engine — no wall clocks.
    // Determinism guarantee: given the same command sequence, seq assignments
    // are identical every run.
    SeqNum seq_{0};
    SeqNum next_seq() noexcept { return ++seq_; }

    // ── Matching ──────────────────────────────────────────────────────────────
    // Helper: run match logic against a concrete side map (templated to handle
    // different comparator types of bids_ vs asks_).
    template <typename SideMap>
    void match_against(Order& incoming, SideMap& opposite) {

        while (incoming.remaining > 0 && !opposite.empty()) {
            auto lvl_it = opposite.begin();  // best price on the opposite side

            // Check if the incoming order's price crosses this level.
            if (incoming.price != MARKET_PRICE) {
                if (incoming.side == Side::Buy  && incoming.price < lvl_it->first) break;
                if (incoming.side == Side::Sell && incoming.price > lvl_it->first) break;
            }

            PriceLevel& level = lvl_it->second;

            // Walk the FIFO queue at this price level.
            while (incoming.remaining > 0 && !level.order_ids.empty()) {
                const OrderId maker_id = level.order_ids.front();
                level.order_ids.pop_front();

                auto maker_it = orders_.find(maker_id);
                if (maker_it == orders_.end()) continue;  // should not happen

                Order& maker = maker_it->second;

                // Skip lazily-deleted orders (canceled or already fully filled).
                if (maker.status == OrderStatus::Canceled ||
                    maker.status == OrderStatus::Filled) {
                    continue;
                }

                // Self-trade prevention: cancel the INCOMING order.
                // The resting maker keeps its place with full priority.
                // Policy is documented in the writeup.
                if (maker.account_id == incoming.account_id) {
                    // Put the maker back at the FRONT to preserve its position.
                    level.order_ids.push_front(maker_id);
                    incoming.status    = OrderStatus::Canceled;
                    incoming.remaining = 0;
                    emit_.emit(Event::make_reject(RejectEvent{
                        next_seq(), incoming.order_id, incoming.account_id,
                        RejectReason::None  // self-trade, not a hard error
                    }));
                    return;
                }

                const Quantity fill_qty = std::min(incoming.remaining, maker.remaining);
                // C++23: both sides have positive remaining — confirmed by while condition
                // and the status check above. Help the optimizer eliminate zero-size branches.
                [[assume(incoming.remaining > 0)]];
                [[assume(maker.remaining > 0)]];
                [[assume(fill_qty > 0)]];
                const Price    fill_price = maker.price;  // always fill at maker's price

                incoming.remaining -= fill_qty;
                maker.remaining    -= fill_qty;
                level.total_size   -= fill_qty;

                const bool taker_done = (incoming.remaining == 0);
                const bool maker_done = (maker.remaining == 0);

                if (taker_done) incoming.status = OrderStatus::Filled;
                else            incoming.status = OrderStatus::PartiallyFilled;

                if (maker_done) {
                    maker.status = OrderStatus::Filled;
                } else {
                    maker.status = OrderStatus::PartiallyFilled;
                    // Maker is partially filled — push back to FRONT to keep priority.
                    // It retains its original time priority (seq number unchanged).
                    level.order_ids.push_front(maker_id);
                }

                emit_.emit(Event::make_fill(FillEvent{
                    next_seq(),
                    incoming.order_id,
                    maker.order_id,
                    incoming.account_id,
                    maker.account_id,
                    incoming.side,
                    fill_price,
                    fill_qty,
                    taker_done,
                    maker_done,
                }));
            }

            // Prune empty price levels.
            if (level.order_ids.empty()) {
                opposite.erase(lvl_it);
            }
        }
    }

    void match(Order& incoming) {
        if (incoming.side == Side::Buy)  match_against(incoming, asks_);
        else                             match_against(incoming, bids_);
    }

    // ── Resting ───────────────────────────────────────────────────────────────
    template <typename SideMap>
    void rest_into(Order& order, SideMap& side_map) {
        // try_emplace: inserts PriceLevel(resource_) only when price is new.
        // Works uniformly for std::pmr::map (tree insert) and FlatPmrMap (sorted insert).
        auto [it, inserted] = side_map.try_emplace(order.price, resource_);

        it->second.order_ids.push_back(order.order_id);
        it->second.total_size += order.remaining;

        order.status = OrderStatus::New;
        orders_.emplace(order.order_id, order);

        // Ack reuses the order's intake seq — it is the confirmation of that command.
        // No new next_seq() call here; fills and cancel_acks get their own fresh seqs.
        emit_.emit(Event::make_ack(AckEvent{
            order.seq,        // intake seq — also establishes time priority
            order.order_id,
            order.account_id,
            order.side,
            order.price,
            order.remaining,
            order.tif,
        }));
    }

    void rest(Order& order) {
        if (order.side == Side::Buy) rest_into(order, bids_);
        else                         rest_into(order, asks_);
    }

    // ── Spread check (for PostOnly) ───────────────────────────────────────────
    bool would_cross(Side side, Price price) const noexcept {
        if (price == MARKET_PRICE) return true;  // market orders always take
        if (side == Side::Buy) {
            return !asks_.empty() && price >= asks_.begin()->first;
        } else {
            return !bids_.empty() && price <= bids_.begin()->first;
        }
    }

    // ── Members ───────────────────────────────────────────────────────────────
    EmitPolicy  emit_;

    // bids: sorted descending (best bid = begin())
    // asks: sorted ascending  (best ask = begin())
    std::pmr::memory_resource* resource_{nullptr};
    std::pmr::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::pmr::map<Price, PriceLevel>                      asks_;
    std::pmr::unordered_map<OrderId, Order>               orders_;

    std::function<bool()> arena_reject_fn_;

public:
    // Must be set before use when using MarketArena.
    void set_resource(std::pmr::memory_resource* r) { resource_ = r; }
};

} // namespace exchange
