#include <gtest/gtest.h>
#include "order_book.hpp"
#include "emit_policy.hpp"
#include "market_arena.hpp"
#include <vector>

using namespace exchange;

struct BookFixture {
    std::vector<Event> events;
    MarketArena        arena;
    OrderBook<CollectingEmitPolicy> book;

    BookFixture()
        : arena(MarketArena::MIN_CAPACITY)
        , book(arena.resource(),
               CollectingEmitPolicy([this](const Event& e){ events.push_back(e); }))
    {
        book.set_resource(arena.resource());
        book.set_arena_reject_fn([this]{ return arena.should_reject_new_orders(); });
    }

    void place(OrderId oid, uint64_t acct, Side side, Price price, Quantity size,
               TIF tif = TIF::GTC) {
        book.place(PlaceCommand{oid, acct, side, price, size, tif});
    }

    void cancel(OrderId oid, uint64_t acct) {
        book.cancel(CancelCommand{oid, acct});
    }

    void clear_events() { events.clear(); }

    const Event& last() const { return events.back(); }

    std::vector<Event> events_of(EventType t) const {
        std::vector<Event> out;
        for (const auto& e : events)
            if (e.type == t) out.push_back(e);
        return out;
    }
};

TEST(Matching, PriceTimePriority) {
    BookFixture f;
    f.place(1, 1, Side::Buy,  100, 10);
    f.place(2, 2, Side::Buy,  100, 10);
    f.clear_events();
    f.place(3, 3, Side::Sell, 100, 5);
    auto fills = f.events_of(EventType::Fill);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].fill.maker_order_id, 1u);
    EXPECT_EQ(fills[0].fill.fill_qty, 5);
}

TEST(Matching, PriceTimePriority_BetterPriceFirst) {
    BookFixture f;
    f.place(1, 1, Side::Buy, 99,  10);
    f.place(2, 2, Side::Buy, 100, 10);
    f.clear_events();
    f.place(3, 3, Side::Sell, 99, 5);
    auto fills = f.events_of(EventType::Fill);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].fill.maker_order_id, 2u);
    EXPECT_EQ(fills[0].fill.fill_price, 100);
}

TEST(Matching, PartialFill_GTC_RemainsWithPriority) {
    BookFixture f;
    f.place(1, 1, Side::Buy, 100, 10);
    f.place(2, 2, Side::Buy, 100,  5);
    f.clear_events();
    f.place(3, 3, Side::Sell, 100, 4);
    {
        auto fills = f.events_of(EventType::Fill);
        ASSERT_EQ(fills.size(), 1u);
        EXPECT_EQ(fills[0].fill.maker_order_id, 1u);
        EXPECT_EQ(fills[0].fill.fill_qty, 4);
        EXPECT_FALSE(fills[0].fill.maker_fully_filled);
    }
    f.clear_events();
    f.place(4, 4, Side::Sell, 100, 3);
    {
        auto fills = f.events_of(EventType::Fill);
        ASSERT_EQ(fills.size(), 1u);
        EXPECT_EQ(fills[0].fill.maker_order_id, 1u);
        EXPECT_EQ(fills[0].fill.fill_qty, 3);
    }
}

TEST(Matching, PartialFill_ThenFullFill_ThenNextLevel) {
    BookFixture f;
    f.place(1, 1, Side::Buy, 100, 5);
    f.place(2, 2, Side::Buy, 100, 3);
    f.clear_events();
    f.place(3, 3, Side::Sell, 100, 8);
    auto fills = f.events_of(EventType::Fill);
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].fill.maker_order_id, 1u);
    EXPECT_EQ(fills[0].fill.fill_qty, 5);
    EXPECT_TRUE(fills[0].fill.maker_fully_filled);
    EXPECT_EQ(fills[1].fill.maker_order_id, 2u);
    EXPECT_EQ(fills[1].fill.fill_qty, 3);
    EXPECT_TRUE(fills[1].fill.taker_fully_filled);
}

TEST(Matching, IOC_FullyFilled) {
    BookFixture f;
    f.place(1, 1, Side::Buy, 100, 10);
    f.clear_events();
    f.place(2, 2, Side::Sell, 100, 5, TIF::IOC);
    auto fills = f.events_of(EventType::Fill);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].fill.fill_qty, 5);
    EXPECT_TRUE(f.events_of(EventType::CancelAck).empty());
}

TEST(Matching, IOC_PartialFill_RemainderCanceled) {
    BookFixture f;
    f.place(1, 1, Side::Buy, 100, 3);
    f.clear_events();
    f.place(2, 2, Side::Sell, 100, 10, TIF::IOC);
    auto fills = f.events_of(EventType::Fill);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].fill.fill_qty, 3);
    auto cancels = f.events_of(EventType::CancelAck);
    ASSERT_EQ(cancels.size(), 1u);
    EXPECT_EQ(cancels[0].cancel_ack.order_id, 2u);
    EXPECT_EQ(cancels[0].cancel_ack.remaining, 7);
}

TEST(Matching, IOC_NoMatch_FullyCanceled) {
    BookFixture f;
    f.place(1, 1, Side::Sell, 100, 5, TIF::IOC);
    auto cancels = f.events_of(EventType::CancelAck);
    ASSERT_EQ(cancels.size(), 1u);
    EXPECT_EQ(cancels[0].cancel_ack.remaining, 5);
    EXPECT_TRUE(f.events_of(EventType::Fill).empty());
}

TEST(Matching, PostOnly_WouldTake_Rejected) {
    BookFixture f;
    f.place(1, 1, Side::Sell, 100, 10);
    f.clear_events();
    f.place(2, 2, Side::Buy, 100, 5, TIF::PostOnly);
    auto rejects = f.events_of(EventType::Reject);
    ASSERT_EQ(rejects.size(), 1u);
    EXPECT_EQ(rejects[0].reject.reason, RejectReason::PostOnlyWouldTake);
    EXPECT_TRUE(f.events_of(EventType::Fill).empty());
    EXPECT_TRUE(f.events_of(EventType::Ack).empty());
}

TEST(Matching, PostOnly_DoesNotCross_Rests) {
    BookFixture f;
    f.place(1, 1, Side::Sell, 100, 10);
    f.clear_events();
    f.place(2, 2, Side::Buy, 99, 5, TIF::PostOnly);
    auto acks = f.events_of(EventType::Ack);
    ASSERT_EQ(acks.size(), 1u);
    EXPECT_EQ(acks[0].ack.order_id, 2u);
    EXPECT_TRUE(f.events_of(EventType::Fill).empty());
    EXPECT_TRUE(f.events_of(EventType::Reject).empty());
}

TEST(Matching, SelfTrade_IncomingCanceled_MakerPreserved) {
    BookFixture f;
    f.place(1, 1, Side::Buy, 100, 10);
    f.clear_events();
    f.place(2, 1, Side::Sell, 100, 5);
    EXPECT_TRUE(f.events_of(EventType::Fill).empty());
    auto rejects = f.events_of(EventType::Reject);
    ASSERT_EQ(rejects.size(), 1u);
    EXPECT_EQ(rejects[0].reject.order_id, 2u);
    f.clear_events();
    f.place(3, 2, Side::Sell, 100, 5);
    auto fills = f.events_of(EventType::Fill);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].fill.maker_order_id, 1u);
    EXPECT_EQ(fills[0].fill.fill_qty, 5);
}

TEST(Matching, Cancel_RestsOnBook_ThenCanceled_DoesNotFill) {
    BookFixture f;
    f.place(1, 1, Side::Buy, 100, 10);
    f.cancel(1, 1);
    f.clear_events();
    f.place(2, 2, Side::Sell, 100, 5);
    EXPECT_TRUE(f.events_of(EventType::Fill).empty());
    auto acks = f.events_of(EventType::Ack);
    ASSERT_EQ(acks.size(), 1u);
}

TEST(Matching, Cancel_UnknownOrderId_Rejected) {
    BookFixture f;
    f.cancel(999, 1);
    auto rejects = f.events_of(EventType::Reject);
    ASSERT_EQ(rejects.size(), 1u);
    EXPECT_EQ(rejects[0].reject.reason, RejectReason::UnknownOrderId);
}

TEST(Matching, Cancel_DuringMatch_PartiallyFilledThenCanceled) {
    BookFixture f;
    f.place(1, 1, Side::Buy, 100, 10);
    f.place(2, 2, Side::Sell, 100, 5);
    f.clear_events();
    f.cancel(1, 1);
    {
        auto cancels = f.events_of(EventType::CancelAck);
        ASSERT_EQ(cancels.size(), 1u);
        EXPECT_EQ(cancels[0].cancel_ack.remaining, 5);
    }
    f.clear_events();
    f.place(3, 3, Side::Sell, 100, 5);
    EXPECT_TRUE(f.events_of(EventType::Fill).empty());
}

TEST(Matching, MarketOrder_WalksMultipleLevels) {
    BookFixture f;
    f.place(1, 1, Side::Sell, 101, 3);
    f.place(2, 2, Side::Sell, 102, 3);
    f.place(3, 3, Side::Sell, 103, 3);
    f.clear_events();
    f.place(4, 4, Side::Buy, MARKET_PRICE, 7, TIF::IOC);
    auto fills = f.events_of(EventType::Fill);
    ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[0].fill.fill_price, 101);
    EXPECT_EQ(fills[0].fill.fill_qty,   3);
    EXPECT_EQ(fills[1].fill.fill_price, 102);
    EXPECT_EQ(fills[1].fill.fill_qty,   3);
    EXPECT_EQ(fills[2].fill.fill_price, 103);
    EXPECT_EQ(fills[2].fill.fill_qty,   1);
    EXPECT_TRUE(f.events_of(EventType::CancelAck).empty());
}

TEST(Matching, SeqNumbers_StrictlyIncreasing) {
    BookFixture f;
    f.place(1, 1, Side::Buy,  100, 5);
    f.place(2, 2, Side::Sell, 100, 5);
    SeqNum prev = 0;
    for (const auto& e : f.events) {
        SeqNum seq = 0;
        switch (e.type) {
            case EventType::Ack:       seq = e.ack.market_seq;        break;
            case EventType::Fill:      seq = e.fill.market_seq;       break;
            case EventType::CancelAck: seq = e.cancel_ack.market_seq; break;
            case EventType::Reject:    seq = e.reject.market_seq;     break;
        }
        EXPECT_GT(seq, prev) << "seq numbers must be strictly increasing";
        prev = seq;
    }
}
