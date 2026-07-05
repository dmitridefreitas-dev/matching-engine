// MapBook — the reference implementation.
//
// Price levels live in std::map (bids descending, asks ascending); each
// level is a std::list in arrival order; an unordered_map from order id to
// (side, price, list iterator) gives O(1) cancel. Every container choice is
// the obvious one, on purpose: this implementation is meant to be *visibly
// correct*, and it is the oracle the optimized FastBook is differentially
// fuzzed against. Roughly: O(log L) to reach a level, O(1) per fill/cancel.
//
// Semantics (identical for every implementation):
//   submit_limit  — match against opposite side while price crosses, FIFO
//                   within a level; any remainder rests. Returns remainder.
//   submit_market — same matching with no price limit; any remainder is
//                   discarded (never rests). Returns unfilled quantity.
//   cancel        — remove a resting order. False if id is not resting.
//   reduce        — lower a resting order's quantity IN PLACE, preserving
//                   time priority (exchange semantics: size down keeps your
//                   place, size up or price change is cancel+replace, which
//                   the caller composes). new_qty == 0 acts as cancel;
//                   new_qty >= current returns false.
//
// Preconditions (enforced by the callers/fuzzer, asserted here in debug):
// order ids are unique across the session, quantities and prices positive.

#pragma once

#include <cassert>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

class MapBook {
public:
    Quantity submit_limit(OrderId id, Side side, Price price, Quantity qty,
                          std::vector<Fill>& fills) {
        assert(qty > 0 && price > 0 && index_.find(id) == index_.end());
        Quantity remaining =
            (side == Side::Buy)
                ? match(asks_, id, qty, [price](Price best) { return best <= price; }, fills)
                : match(bids_, id, qty, [price](Price best) { return best >= price; }, fills);
        if (remaining > 0) rest(id, side, price, remaining);
        return remaining;
    }

    Quantity submit_market(OrderId id, Side side, Quantity qty, std::vector<Fill>& fills) {
        assert(qty > 0);
        return (side == Side::Buy)
                   ? match(asks_, id, qty, [](Price) { return true; }, fills)
                   : match(bids_, id, qty, [](Price) { return true; }, fills);
    }

    bool cancel(OrderId id) {
        auto found = index_.find(id);
        if (found == index_.end()) return false;
        erase_resting(found);
        return true;
    }

    bool reduce(OrderId id, Quantity new_qty) {
        auto found = index_.find(id);
        if (found == index_.end()) return false;
        Quantity current = found->second.node->qty;
        if (new_qty >= current) return false;
        if (new_qty == 0) {
            erase_resting(found);
            return true;
        }
        found->second.node->qty = new_qty;
        return true;
    }

    bool has_bid() const { return !bids_.empty(); }
    bool has_ask() const { return !asks_.empty(); }
    Price best_bid() const { return bids_.begin()->first; }
    Price best_ask() const { return asks_.begin()->first; }
    std::size_t open_orders() const { return index_.size(); }

    std::vector<LevelSnapshot> snapshot(Side side) const {
        std::vector<LevelSnapshot> out;
        auto emit = [&out](Price price, const Level& level) {
            LevelSnapshot snap{price, {}};
            for (const Resting& r : level) snap.orders.emplace_back(r.id, r.qty);
            out.push_back(std::move(snap));
        };
        if (side == Side::Buy) {
            for (const auto& [price, level] : bids_) emit(price, level);
        } else {
            for (const auto& [price, level] : asks_) emit(price, level);
        }
        return out;
    }

private:
    struct Resting {
        OrderId id;
        Quantity qty;
    };
    using Level = std::list<Resting>;

    struct Locator {
        Side side;
        Price price;
        Level::iterator node;
    };

    template <typename OppositeMap, typename Crosses>
    Quantity match(OppositeMap& opposite, OrderId taker, Quantity qty, Crosses crosses,
                   std::vector<Fill>& fills) {
        while (qty > 0 && !opposite.empty()) {
            auto level_it = opposite.begin();
            if (!crosses(level_it->first)) break;
            Level& level = level_it->second;
            while (qty > 0 && !level.empty()) {
                Resting& maker = level.front();
                Quantity traded = maker.qty < qty ? maker.qty : qty;
                fills.push_back({taker, maker.id, level_it->first, traded});
                qty -= traded;
                maker.qty -= traded;
                if (maker.qty == 0) {
                    index_.erase(maker.id);
                    level.pop_front();
                }
            }
            if (level.empty()) opposite.erase(level_it);
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Quantity qty) {
        Level& level = (side == Side::Buy) ? bids_[price] : asks_[price];
        level.push_back({id, qty});
        index_.emplace(id, Locator{side, price, std::prev(level.end())});
    }

    void erase_resting(std::unordered_map<OrderId, Locator>::iterator found) {
        const Locator& loc = found->second;
        if (loc.side == Side::Buy) {
            auto level_it = bids_.find(loc.price);
            level_it->second.erase(loc.node);
            if (level_it->second.empty()) bids_.erase(level_it);
        } else {
            auto level_it = asks_.find(loc.price);
            level_it->second.erase(loc.node);
            if (level_it->second.empty()) asks_.erase(level_it);
        }
        index_.erase(found);
    }

    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level> asks_;
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace lob
