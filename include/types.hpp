#pragma once
#include <cstdint>
#include <string>

namespace exchange {

// ── Enumerations ────────────────────────────────────────────────────────────

enum class Side : uint8_t { Buy, Sell };

enum class TIF : uint8_t {
    GTC,       // Good-Till-Cancel: rests on book until filled or explicitly canceled
    IOC,       // Immediate-Or-Cancel: fill what matches now, cancel remainder
    PostOnly,  // Rejected if it would cross the spread (take liquidity); rests otherwise
};

enum class OrderStatus : uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Canceled,
    Rejected,
};

enum class EventType : uint8_t {
    Ack,        // Order accepted and resting on the book
    Fill,       // A match occurred (partial or full)
    CancelAck,  // Cancel confirmed
    Reject,     // Order rejected before touching the book
};

enum class RejectReason : uint8_t {
    None,
    MemoryExhausted,  // Arena at 99% watermark — new orders blocked, cancels still accepted
    PostOnlyWouldTake, // PostOnly order would have crossed the spread
    UnknownOrderId,    // Cancel for an order that doesn't exist or is already terminal
};

// ── Core types ───────────────────────────────────────────────────────────────

// Prices are represented as int64_t fixed-point integers (e.g. price in cents or ticks).
// No floating-point arithmetic anywhere in the matching path — avoids rounding
// non-determinism and makes map keys exact.
using Price    = int64_t;
using Quantity = int64_t;
using OrderId  = uint64_t;
using SeqNum   = uint64_t;

// Price sentinel for market orders (no price constraint).
static constexpr Price MARKET_PRICE = 0;

struct Order {
    OrderId    order_id;
    uint64_t   account_id;
    Side       side;
    Price      price;        // MARKET_PRICE for market orders
    Quantity   size;         // original size
    Quantity   remaining;    // decremented on each partial fill
    TIF        tif;
    SeqNum     seq;          // monotonic sequence number assigned at intake — establishes time priority
    OrderStatus status;
};

// ── Events emitted by the matching engine ────────────────────────────────────
// The engine never exposes a query interface. All state changes are observable
// exclusively via this event stream. A downstream read model reconstructs book
// state by consuming these events.

struct FillEvent {
    SeqNum   market_seq;     // per-market monotonic seq — enables gap detection on replay
    OrderId  taker_order_id;
    OrderId  maker_order_id;
    uint64_t taker_account;
    uint64_t maker_account;
    Side     taker_side;
    Price    fill_price;     // always the maker's (resting) price — price-time priority
    Quantity fill_qty;
    bool     taker_fully_filled;
    bool     maker_fully_filled;
};

struct AckEvent {
    SeqNum   market_seq;
    OrderId  order_id;
    uint64_t account_id;
    Side     side;
    Price    price;
    Quantity size;
    TIF      tif;
};

struct CancelAckEvent {
    SeqNum   market_seq;
    OrderId  order_id;
    uint64_t account_id;
    Quantity remaining;  // how much was left when canceled
};

struct RejectEvent {
    SeqNum       market_seq;
    OrderId      order_id;
    uint64_t     account_id;
    RejectReason reason;
};

struct Event {
    EventType type;
    union {
        FillEvent      fill;
        AckEvent       ack;
        CancelAckEvent cancel_ack;
        RejectEvent    reject;
    };

    static Event make_fill(FillEvent f)           { Event e; e.type = EventType::Fill;      e.fill       = f; return e; }
    static Event make_ack(AckEvent a)             { Event e; e.type = EventType::Ack;       e.ack        = a; return e; }
    static Event make_cancel_ack(CancelAckEvent c){ Event e; e.type = EventType::CancelAck; e.cancel_ack = c; return e; }
    static Event make_reject(RejectEvent r)       { Event e; e.type = EventType::Reject;    e.reject     = r; return e; }
};

// ── Commands accepted by the engine ──────────────────────────────────────────

enum class CommandType : uint8_t { Place, Cancel };

struct PlaceCommand {
    OrderId  order_id;
    uint64_t account_id;
    Side     side;
    Price    price;    // MARKET_PRICE for market orders
    Quantity size;
    TIF      tif;
};

struct CancelCommand {
    OrderId  order_id;
    uint64_t account_id;  // must match the resting order's account
};

struct Command {
    CommandType type;
    union {
        PlaceCommand  place;
        CancelCommand cancel;
    };

    static Command make_place(PlaceCommand p)   { Command c; c.type = CommandType::Place;  c.place  = p; return c; }
    static Command make_cancel(CancelCommand cc){ Command c; c.type = CommandType::Cancel; c.cancel = cc; return c; }
};

} // namespace exchange
