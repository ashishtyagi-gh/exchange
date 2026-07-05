#pragma once
#include "types.hpp"
#include <iostream>
#include <string>
#include <functional>
#include <utility>  // std::unreachable (C++23)

namespace exchange {

// Emit policies — injected into OrderBook<EmitPolicy> via template parameter.
// No default is provided. Every instantiation site must explicitly choose a policy.
// This makes it impossible to accidentally emit to stdout in a benchmark or test.
//
// StdoutEmitPolicy:
//   Writes JSON-lines directly from the engine thread (synchronous).
//   stdout I/O is in the critical path — this is the right choice for the CLI
//   take-home, and the known cost is documented. Not for production matching.
//
// NullEmitPolicy:
//   Drops all events. Used for throughput benchmarks so I/O cost does not
//   contaminate matching measurements. Also used in unit tests that assert
//   via a CollectingEmitPolicy instead.
//
// CollectingEmitPolicy:
//   Stores events in a user-supplied callback. Used in tests to assert on
//   emitted events without stdout noise.
//
// QueueEmitPolicy<Queue>:
//   Enqueues events for a dedicated writer thread (e.g. a Kafka producer,
//   a network sender, a file logger). The engine never blocks on I/O.
//   The queue should be SPSC — the engine thread is the sole producer.

// ── Helpers ──────────────────────────────────────────────────────────────────

inline std::string side_str(Side s)   { return s == Side::Buy ? "buy" : "sell"; }
inline std::string tif_str(TIF t)  {
    switch (t) {
        case TIF::GTC:      return "gtc";
        case TIF::IOC:      return "ioc";
        case TIF::PostOnly: return "post_only";
    }
    std::unreachable();  // all TIF enumerators handled above
}
inline std::string reject_reason_str(RejectReason r) {
    switch (r) {
        case RejectReason::None:              return "none";
        case RejectReason::MemoryExhausted:   return "memory_exhausted";
        case RejectReason::PostOnlyWouldTake: return "post_only_would_take";
        case RejectReason::UnknownOrderId:    return "unknown_order_id";
    }
    std::unreachable();  // all RejectReason enumerators handled above
}

inline void emit_json(const Event& e) {
    switch (e.type) {
        case EventType::Fill:
            std::cout
                << "{\"type\":\"fill\""
                << ",\"seq\":"          << e.fill.market_seq
                << ",\"taker_order_id\":" << e.fill.taker_order_id
                << ",\"maker_order_id\":" << e.fill.maker_order_id
                << ",\"taker_account\":" << e.fill.taker_account
                << ",\"maker_account\":" << e.fill.maker_account
                << ",\"taker_side\":\""  << side_str(e.fill.taker_side) << "\""
                << ",\"price\":"        << e.fill.fill_price
                << ",\"qty\":"          << e.fill.fill_qty
                << ",\"taker_done\":"   << (e.fill.taker_fully_filled ? "true" : "false")
                << ",\"maker_done\":"   << (e.fill.maker_fully_filled ? "true" : "false")
                << "}\n";
            break;
        case EventType::Ack:
            std::cout
                << "{\"type\":\"ack\""
                << ",\"seq\":"       << e.ack.market_seq
                << ",\"order_id\":"  << e.ack.order_id
                << ",\"account\":"   << e.ack.account_id
                << ",\"side\":\""    << side_str(e.ack.side) << "\""
                << ",\"price\":"     << e.ack.price
                << ",\"size\":"      << e.ack.size
                << ",\"tif\":\""     << tif_str(e.ack.tif) << "\""
                << "}\n";
            break;
        case EventType::CancelAck:
            std::cout
                << "{\"type\":\"cancel_ack\""
                << ",\"seq\":"       << e.cancel_ack.market_seq
                << ",\"order_id\":"  << e.cancel_ack.order_id
                << ",\"account\":"   << e.cancel_ack.account_id
                << ",\"remaining\":" << e.cancel_ack.remaining
                << "}\n";
            break;
        case EventType::Reject:
            std::cout
                << "{\"type\":\"reject\""
                << ",\"seq\":"      << e.reject.market_seq
                << ",\"order_id\":" << e.reject.order_id
                << ",\"account\":"  << e.reject.account_id
                << ",\"reason\":\"" << reject_reason_str(e.reject.reason) << "\""
                << "}\n";
            break;
    }
    std::unreachable();  // all EventType enumerators handled above
}

// ── Policy implementations ────────────────────────────────────────────────────

struct StdoutEmitPolicy {
    void emit(const Event& e) const { emit_json(e); }
};

struct NullEmitPolicy {
    // Drops all events. Zero overhead — the call is a no-op and will be
    // inlined and eliminated by the optimizer. Used for benchmarks.
    void emit(const Event&) const noexcept {}
};

struct CollectingEmitPolicy {
    // Invokes a user-supplied callback for each event. Used in tests so
    // assertions can be made on the exact event sequence without stdout.
    explicit CollectingEmitPolicy(std::function<void(const Event&)> cb)
        : callback_(std::move(cb)) {}

    void emit(const Event& e) const { callback_(e); }

private:
    std::function<void(const Event&)> callback_;
};

template <typename Queue>
struct QueueEmitPolicy {
    // Enqueues events for an async writer thread. The engine never blocks on I/O.
    // Queue must be SPSC — the engine thread is the sole producer.
    explicit QueueEmitPolicy(Queue& q) : queue_(q) {}
    void emit(const Event& e) { queue_.push(e); }

private:
    Queue& queue_;
};

} // namespace exchange
