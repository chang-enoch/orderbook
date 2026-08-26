#include "orderBook.h"

#include <cstdio>
#include <vector>

namespace {

void Print(const OrderBook& book, const char* label) {
    std::vector<std::pair<Price, Qty>> bids, asks;
    book.Snapshot(bids, asks);
    std::printf("\n--- %s ---\n", label);
    std::printf("  asks (best last):\n");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        std::printf("    %8lld  x %llu\n", (long long)it->first,
                    (unsigned long long)it->second);
    std::printf("  bids (best first):\n");
    for (const auto& [p, q] : bids)
        std::printf("    %8lld  x %llu\n", (long long)p, (unsigned long long)q);
    std::printf("  best bid/ask: %lld / %lld   resting orders: %zu\n",
                (long long)book.BestBid(), (long long)book.BestAsk(),
                book.OrderCount());
}

void PrintTrades(const std::vector<Trade>& trades, std::size_t from) {
    for (std::size_t i = from; i < trades.size(); ++i)
        std::printf("  TRADE taker=%llu maker=%llu  %llu @ %lld\n",
                    (unsigned long long)trades[i].taker,
                    (unsigned long long)trades[i].maker,
                    (unsigned long long)trades[i].qty,
                    (long long)trades[i].price);
}

}  // namespace

int main() {
    OrderBook book;
    std::vector<Trade> trades;

    // Build a two-sided book.
    book.AddOrder(1, Side::Buy,  100, 50, TimeInForce::Gtc, trades);
    book.AddOrder(2, Side::Buy,  100, 30, TimeInForce::Gtc, trades);
    book.AddOrder(3, Side::Buy,   99, 80, TimeInForce::Gtc, trades);
    book.AddOrder(4, Side::Sell, 101, 40, TimeInForce::Gtc, trades);
    book.AddOrder(5, Side::Sell, 102, 60, TimeInForce::Gtc, trades);
    Print(book, "resting book");

    // Aggressive sell sweeps the 100 level FIFO: order 1 first, then order 2.
    std::size_t mark = trades.size();
    std::printf("\nsell 60 @ 100 (crosses):\n");
    book.AddOrder(6, Side::Sell, 100, 60, TimeInForce::Gtc, trades);
    PrintTrades(trades, mark);
    Print(book, "after sweep");

    // Size-down keeps priority; a reprice goes to the back of the new level.
    book.ModifyOrder(3, 99, 50, trades);
    std::printf("\nmodify 3 -> 50 @ 99 (size down, keeps priority)\n");
    Print(book, "after size-down");

    mark = trades.size();
    std::printf("\nmodify 3 -> 60 @ 101 (reprice, crosses):\n");
    book.ModifyOrder(3, 101, 60, trades);
    PrintTrades(trades, mark);
    Print(book, "after reprice");

    // IOC discards its remainder instead of resting.
    mark = trades.size();
    std::printf("\nIOC buy 100 @ 102:\n");
    book.AddOrder(7, Side::Buy, 102, 100, TimeInForce::Ioc, trades);
    PrintTrades(trades, mark);
    Print(book, "after IOC");

    book.CancelOrder(2);
    std::printf("\ncancel 2\n");
    Print(book, "final");
    return 0;
}
