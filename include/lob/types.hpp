// Core types shared by every book implementation.
//
// Prices are integer ticks and quantities are integer units throughout —
// floating point has no business inside a matching engine. Fills are the
// canonical output: two engines are considered equivalent exactly when
// identical input streams produce identical fill sequences, identical
// return values, and identical book snapshots.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;      // integer ticks, > 0
using Quantity = std::uint32_t;  // integer units, > 0

enum class Side : std::uint8_t { Buy, Sell };

inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

// One execution. Price is always the maker's (resting) price: an aggressive
// buy at 105 hitting an ask resting at 100 trades at 100 — price improvement
// goes to the taker, exactly as on a real exchange.
struct Fill {
    OrderId taker;
    OrderId maker;
    Price price;
    Quantity quantity;

    friend bool operator==(const Fill& a, const Fill& b) {
        return a.taker == b.taker && a.maker == b.maker && a.price == b.price &&
               a.quantity == b.quantity;
    }
};

// Snapshot of one price level, orders in FIFO (time-priority) sequence.
// Used by tests to compare implementations; not a hot-path type.
struct LevelSnapshot {
    Price price;
    std::vector<std::pair<OrderId, Quantity>> orders;

    friend bool operator==(const LevelSnapshot& a, const LevelSnapshot& b) {
        return a.price == b.price && a.orders == b.orders;
    }
};

}  // namespace lob
