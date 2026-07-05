#include <gtest/gtest.h>
#include "order_book.hpp"
#include "consumer.hpp"
#include "emit_policy.hpp"
#include "market_arena.hpp"
#include "idle_policy.hpp"
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <mutex>

using namespace exchange;

TEST(MultiMarket, MarketsAreIndependent) {
    MarketArena btc_arena(MarketArena::MIN_CAPACITY);
    MarketArena slv_arena(MarketArena::MIN_CAPACITY);

    std::vector<Event> btc_events, slv_events;
    std::mutex btc_mu, slv_mu;

    using Book = OrderBook<CollectingEmitPolicy>;

    auto btc_book = std::make_unique<Book>(
        btc_arena.resource(),
        CollectingEmitPolicy([&](const Event& e){
            std::lock_guard<std::mutex> lk(btc_mu);
            btc_events.push_back(e);
        }));
    btc_book->set_resource(btc_arena.resource());
    btc_book->set_arena_reject_fn([&]{ return btc_arena.should_reject_new_orders(); });

    auto slv_book = std::make_unique<Book>(
        slv_arena.resource(),
        CollectingEmitPolicy([&](const Event& e){
            std::lock_guard<std::mutex> lk(slv_mu);
            slv_events.push_back(e);
        }));
    slv_book->set_resource(slv_arena.resource());
    slv_book->set_arena_reject_fn([&]{ return slv_arena.should_reject_new_orders(); });

    MarketQueue btc_queue, slv_queue;

    MarketHandle btc_handle{&btc_queue, [&](const Command& cmd){
        if (cmd.type == CommandType::Place)  btc_book->place(cmd.place);
        else                                  btc_book->cancel(cmd.cancel);
    }};
    MarketHandle slv_handle{&slv_queue, [&](const Command& cmd){
        if (cmd.type == CommandType::Place)  slv_book->place(cmd.place);
        else                                  slv_book->cancel(cmd.cancel);
    }};

    Consumer<BackoffPolicy> btc_consumer(btc_handle);
    Consumer<BackoffPolicy> slv_consumer(slv_handle);
    btc_consumer.start();
    slv_consumer.start();

    btc_queue.push(Command::make_place({1, 10, Side::Buy,  50000, 2, TIF::GTC}));
    slv_queue.push(Command::make_place({2, 20, Side::Sell, 25,    5, TIF::GTC}));
    btc_queue.push(Command::make_place({3, 11, Side::Sell, 50000, 2, TIF::GTC}));
    slv_queue.push(Command::make_place({4, 21, Side::Buy,  25,    5, TIF::GTC}));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    btc_consumer.stop();
    slv_consumer.stop();

    {
        std::lock_guard<std::mutex> lk(btc_mu);
        int btc_fills = 0;
        for (const auto& e : btc_events)
            if (e.type == EventType::Fill) ++btc_fills;
        EXPECT_EQ(btc_fills, 1) << "BTC should have exactly 1 fill";
    }
    {
        std::lock_guard<std::mutex> lk(slv_mu);
        int slv_fills = 0;
        for (const auto& e : slv_events)
            if (e.type == EventType::Fill) ++slv_fills;
        EXPECT_EQ(slv_fills, 1) << "SILVER should have exactly 1 fill";
    }
    {
        std::lock_guard<std::mutex> lk(btc_mu);
        for (const auto& e : btc_events) {
            if (e.type == EventType::Fill) {
                EXPECT_TRUE(e.fill.taker_order_id == 1 || e.fill.taker_order_id == 3 ||
                            e.fill.maker_order_id == 1 || e.fill.maker_order_id == 3)
                    << "BTC fill references unexpected order id";
            }
        }
    }
}

TEST(MultiMarket, SeqNumbersArePerMarket) {
    MarketArena a1(MarketArena::MIN_CAPACITY), a2(MarketArena::MIN_CAPACITY);
    std::vector<Event> e1, e2;

    OrderBook<CollectingEmitPolicy> b1(a1.resource(),
        CollectingEmitPolicy([&](const Event& e){ e1.push_back(e); }));
    b1.set_resource(a1.resource());

    OrderBook<CollectingEmitPolicy> b2(a2.resource(),
        CollectingEmitPolicy([&](const Event& e){ e2.push_back(e); }));
    b2.set_resource(a2.resource());

    b1.place({1, 1, Side::Buy, 100, 5, TIF::GTC});
    b2.place({2, 2, Side::Buy, 200, 5, TIF::GTC});

    ASSERT_FALSE(e1.empty());
    ASSERT_FALSE(e2.empty());

    EXPECT_EQ(e1.front().ack.market_seq, 1u) << "Market 1 first event should be seq=1";
    EXPECT_EQ(e2.front().ack.market_seq, 1u) << "Market 2 first event should be seq=1 (independent)";
}
