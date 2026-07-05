#include "types.hpp"
#include "order_book.hpp"
#include "market_arena.hpp"
#include "consumer.hpp"
#include "emit_policy.hpp"
#include "idle_policy.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <cstdlib>

// ── CLI entry point ───────────────────────────────────────────────────────────
//
// Reads JSON-line commands from stdin, routes them to the appropriate market's
// SPSC queue, emits events to stdout.
//
// Input format (one JSON object per line):
//
//   Place order:
//     {"cmd":"place","market":"BTC","order_id":1,"account_id":42,"side":"buy",
//      "price":10000,"size":5,"tif":"gtc"}
//     price=0 means market order
//     tif: "gtc" | "ioc" | "post_only"
//
//   Cancel order:
//     {"cmd":"cancel","market":"BTC","order_id":1,"account_id":42}
//
// Output: JSON-lines events (fill, ack, cancel_ack, reject) written to stdout.
//
// The engine itself never touches stdout — all output flows through StdoutEmitPolicy.
//
// For simplicity this CLI is single-threaded (no separate consumer thread).
// Commands are processed synchronously in the main loop. The SPSC queue and
// Consumer<> infrastructure is exercised in the multi-market benchmark and tests.

using namespace exchange;

// Minimal JSON field extractor — no dependencies, sufficient for the test harness.
static std::string get_str(const std::string& json, const std::string& key) {
    // Looks for "key":"value" or "key":value
    const std::string quoted_key = "\"" + key + "\":";
    auto pos = json.find(quoted_key);
    if (pos == std::string::npos) return "";
    pos += quoted_key.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (json[pos] == '"') {
        ++pos;
        auto end = json.find('"', pos);
        return json.substr(pos, end - pos);
    }
    // numeric value
    auto end = json.find_first_of(",}", pos);
    return json.substr(pos, end - pos);
}

static int64_t get_int(const std::string& json, const std::string& key) {
    const std::string s = get_str(json, key);
    if (s.empty()) return 0;
    return std::stoll(s);
}

int main() {
    // Each market gets its own arena and order book.
    // In a real system, capacity would come from a config file.
    struct MarketEntry {
        MarketArena                          arena;
        std::unique_ptr<OrderBook<StdoutEmitPolicy>> book;
        MarketQueue                          queue;

        MarketEntry()
            : arena(MarketArena::DEFAULT_CAPACITY)
            , book(nullptr)
        {
            book = std::make_unique<OrderBook<StdoutEmitPolicy>>(
                arena.resource(), StdoutEmitPolicy{});
            book->set_resource(arena.resource());
            book->set_arena_reject_fn([this]{ return arena.should_reject_new_orders(); });
        }
    };

    std::unordered_map<std::string, std::unique_ptr<MarketEntry>> markets;

    auto get_or_create = [&](const std::string& symbol) -> MarketEntry& {
        auto it = markets.find(symbol);
        if (it == markets.end()) {
            markets.emplace(symbol, std::make_unique<MarketEntry>());
            it = markets.find(symbol);
        }
        return *it->second;
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty() || line[0] == '#') continue;  // skip blank lines and comments

        const std::string cmd_type = get_str(line, "cmd");
        const std::string market   = get_str(line, "market");

        if (market.empty()) {
            std::cerr << "{\"error\":\"missing 'market' field\",\"input\":" << line << "}\n";
            continue;
        }

        MarketEntry& entry = get_or_create(market);

        if (cmd_type == "place") {
            const std::string side_str = get_str(line, "side");
            const std::string tif_str  = get_str(line, "tif");

            Side side = (side_str == "buy") ? Side::Buy : Side::Sell;

            TIF tif = TIF::GTC;
            if      (tif_str == "ioc")       tif = TIF::IOC;
            else if (tif_str == "post_only") tif = TIF::PostOnly;

            PlaceCommand cmd{
                static_cast<OrderId>(get_int(line, "order_id")),
                static_cast<uint64_t>(get_int(line, "account_id")),
                side,
                static_cast<Price>(get_int(line, "price")),
                static_cast<Quantity>(get_int(line, "size")),
                tif,
            };
            entry.book->place(cmd);

        } else if (cmd_type == "cancel") {
            CancelCommand cmd{
                static_cast<OrderId>(get_int(line, "order_id")),
                static_cast<uint64_t>(get_int(line, "account_id")),
            };
            entry.book->cancel(cmd);

        } else {
            std::cerr << "{\"error\":\"unknown cmd\",\"input\":\"" << line << "\"}\n";
        }
    }

    return 0;
}
