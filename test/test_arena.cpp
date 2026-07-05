#include <gtest/gtest.h>
#include "market_arena.hpp"
#include "order_book.hpp"
#include "emit_policy.hpp"

using namespace exchange;

TEST(Arena, ConstructionDefaults) {
    MarketArena arena;
    EXPECT_EQ(arena.capacity(), MarketArena::DEFAULT_CAPACITY);
    // With the pool layer, ~528 bytes are committed at construction for pool's
    // internal per-size-class descriptor array. The arena must not be near
    // the rejection watermark.
    EXPECT_FALSE(arena.should_reject_new_orders());
    EXPECT_LT(arena.used_bytes(), 4096u);  // pool init overhead is always tiny
}

TEST(Arena, ConstructionCustomCapacity) {
    MarketArena arena(32ULL * 1024 * 1024);  // 32 MB
    EXPECT_EQ(arena.capacity(), 32ULL * 1024 * 1024);
}

TEST(Arena, BelowMinCapacity_Throws) {
    EXPECT_THROW(MarketArena(1024), std::invalid_argument);
}

TEST(Arena, InvalidRejectRatio_Throws) {
    EXPECT_THROW(MarketArena(MarketArena::MIN_CAPACITY, 0.0), std::invalid_argument);
    EXPECT_THROW(MarketArena(MarketArena::MIN_CAPACITY, 1.0), std::invalid_argument);
    EXPECT_THROW(MarketArena(MarketArena::MIN_CAPACITY, 1.5), std::invalid_argument);
}

TEST(Arena, ShouldNotRejectWhenFresh) {
    MarketArena arena(MarketArena::MIN_CAPACITY);
    EXPECT_FALSE(arena.should_reject_new_orders());
}

// Fill the arena past the rejection watermark by placing many resting orders.
// New Place orders must be rejected; Cancel must still be accepted.
TEST(Arena, WatermarkRejectsNewOrders_AllowsCancel) {
    // Use a 70% watermark (not the default 99%) so the safety reserve is large
    // enough to absorb a single unordered_map rehash allocation.
    // std::pmr::unordered_map rehashes allocate a bucket array that can exceed
    // 512KB at ~50K entries. With a 16MB arena, the 1% default reserve (163KB)
    // is too tight; 30% = 4.8MB comfortably covers any single rehash.
    // Production arenas (100MB+) have adequate reserve at the 99% default.
    MarketArena arena(MarketArena::MIN_CAPACITY, 0.70);

    std::vector<Event> events;
    using Book = OrderBook<CollectingEmitPolicy>;
    Book book(arena.resource(),
              CollectingEmitPolicy([&](const Event& e){ events.push_back(e); }));
    book.set_resource(arena.resource());
    book.set_arena_reject_fn([&]{ return arena.should_reject_new_orders(); });

    // Place orders until we hit the watermark.
    OrderId oid = 1;
    uint64_t acct = 1;
    bool got_rejection = false;

    for (int i = 0; i < 500000; ++i) {
        book.place(PlaceCommand{oid++, acct, Side::Buy, static_cast<Price>(i + 1), 1, TIF::GTC});
        if (!events.empty() && events.back().type == EventType::Reject &&
            events.back().reject.reason == RejectReason::MemoryExhausted) {
            got_rejection = true;
            break;
        }
    }

    EXPECT_TRUE(got_rejection) << "Expected MemoryExhausted rejection before 500k orders";

    // After hitting the watermark, cancel one of the resting orders (order id 1).
    // Cancel must succeed regardless of watermark.
    events.clear();
    book.cancel(CancelCommand{1, acct});

    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, EventType::CancelAck)
        << "Cancel must be accepted even when arena is exhausted";
}
