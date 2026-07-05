// FastBook — the optimized implementation, differentially fuzzed against
// MapBook (identical semantics; see map_book.hpp for the contract).
//
// Where the time goes in MapBook, and what this does about each:
//
//   std::map node walk to reach a level   ->  contiguous PRICE LADDER:
//       one array slot per tick in [min_price, max_price], indexed by
//       (price - min_price). Reaching any level is one add and one load;
//       best bid/ask are maintained as ladder indices, re-found via the
//       occupancy bitmap when a best level empties.
//
//   one heap allocation per resting order  ->  OBJECT POOL + free list:
//       orders live in one std::vector<Node>; freed slots are recycled
//       LIFO. No allocation in steady state, and neighbouring orders are
//       likely to share cache lines.
//
//   std::list scattered nodes              ->  INTRUSIVE doubly-linked FIFO
//       inside the pool: each Node carries prev/next pool indices, each
//       ladder slot carries head/tail. O(1) append, O(1) unlink from the
//       middle (cancels), no pointer chasing across the heap.
//
//   two measured weaknesses in v1                ->  fixed in v2:
//       the LOBSTER replay showed p99 latency WORSE than MapBook's — the
//       best-index scan walked hundreds of empty ticks on a sparse
//       $0.0001-tick book. A BITMAP SUMMARY (one bit per price level,
//       scanned 64 ticks at a time with countr_zero/countl_zero) makes
//       finding the next occupied level O(gap/64) with tiny constants.
//       And the id -> slot index was a std::unordered_map — per-node
//       allocation and bucket pointer-chasing on EVERY submit/fill/cancel.
//       Replaced by lob::IdMap (open addressing, linear probing,
//       backward-shift deletion; see id_map.hpp).
//
// Honest notes, so the benchmark numbers are read correctly: the ladder
// needs price bounds up front (fine for a day session on one instrument,
// and out-of-range prices throw rather than corrupt); order ids must be
// nonzero (IdMap's empty sentinel); and everything here is single-threaded
// matching only — no networking, sessions, or persistence.

#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "lob/id_map.hpp"
#include "lob/types.hpp"

namespace lob {

class FastBook {
    static constexpr std::uint32_t NIL = std::numeric_limits<std::uint32_t>::max();

public:
    FastBook(Price min_price, Price max_price, std::size_t expected_orders = 1 << 20)
        : min_price_(min_price),
          ladder_size_(static_cast<std::int64_t>(max_price - min_price + 1)),
          bid_levels_(static_cast<std::size_t>(ladder_size_)),
          ask_levels_(static_cast<std::size_t>(ladder_size_)),
          bid_bits_(static_cast<std::size_t>((ladder_size_ + 63) / 64), 0),
          ask_bits_(static_cast<std::size_t>((ladder_size_ + 63) / 64), 0),
          best_bid_(-1),
          best_ask_(ladder_size_),
          index_(expected_orders) {
        if (min_price <= 0 || max_price < min_price)
            throw std::invalid_argument("FastBook: bad price bounds");
        pool_.reserve(expected_orders);
    }

    Quantity submit_limit(OrderId id, Side side, Price price, Quantity qty,
                          std::vector<Fill>& fills) {
        assert(qty > 0 && id != 0 && index_.find(id) == nullptr);
        const std::int64_t limit_idx = to_index(price);
        Quantity remaining = (side == Side::Buy)
                                 ? match_asks(id, qty, limit_idx, fills)
                                 : match_bids(id, qty, limit_idx, fills);
        if (remaining > 0) rest(id, side, limit_idx, remaining);
        return remaining;
    }

    Quantity submit_market(OrderId id, Side side, Quantity qty, std::vector<Fill>& fills) {
        assert(qty > 0);
        return (side == Side::Buy) ? match_asks(id, qty, ladder_size_ - 1, fills)
                                   : match_bids(id, qty, 0, fills);
    }

    bool cancel(OrderId id) {
        const std::uint32_t* slot = index_.find(id);
        if (slot == nullptr) return false;
        unlink_and_free(*slot);
        index_.erase(id);
        return true;
    }

    bool reduce(OrderId id, Quantity new_qty) {
        const std::uint32_t* slot = index_.find(id);
        if (slot == nullptr) return false;
        Node& node = pool_[*slot];
        if (new_qty >= node.qty) return false;
        if (new_qty == 0) {
            unlink_and_free(*slot);
            index_.erase(id);
            return true;
        }
        node.qty = new_qty;
        return true;
    }

    bool has_bid() const { return best_bid_ >= 0; }
    bool has_ask() const { return best_ask_ < ladder_size_; }
    Price best_bid() const { return min_price_ + best_bid_; }
    Price best_ask() const { return min_price_ + best_ask_; }
    std::size_t open_orders() const { return index_.size(); }

    std::vector<LevelSnapshot> snapshot(Side side) const {
        std::vector<LevelSnapshot> out;
        auto emit = [this, &out](const Level& level, std::int64_t idx) {
            if (level.head == NIL) return;
            LevelSnapshot snap{min_price_ + idx, {}};
            for (std::uint32_t n = level.head; n != NIL; n = pool_[n].next)
                snap.orders.emplace_back(pool_[n].id, pool_[n].qty);
            out.push_back(std::move(snap));
        };
        if (side == Side::Buy) {
            for (std::int64_t i = best_bid_; i >= 0; --i) emit(bid_levels_[i], i);
        } else {
            for (std::int64_t i = best_ask_; i < ladder_size_; ++i) emit(ask_levels_[i], i);
        }
        return out;
    }

private:
    struct Node {
        OrderId id;
        Quantity qty;
        std::uint32_t prev;
        std::uint32_t next;
        std::int64_t ladder_idx;  // sign of side_flag below decides which ladder
        Side side;
    };

    struct Level {
        std::uint32_t head = NIL;
        std::uint32_t tail = NIL;
    };

    std::int64_t to_index(Price price) const {
        const std::int64_t idx = price - min_price_;
        if (idx < 0 || idx >= ladder_size_)
            throw std::out_of_range("FastBook: price outside ladder bounds");
        return idx;
    }

    // Match an aggressive BUY against asks from best upward while the ask
    // level is at or below the taker's limit index.
    Quantity match_asks(OrderId taker, Quantity qty, std::int64_t limit_idx,
                        std::vector<Fill>& fills) {
        while (qty > 0 && best_ask_ < ladder_size_ && best_ask_ <= limit_idx) {
            qty = consume_level(ask_levels_[best_ask_], taker, qty, min_price_ + best_ask_, fills);
            if (ask_levels_[best_ask_].head == NIL) {
                clear_bit(ask_bits_, best_ask_);
                best_ask_ = next_occupied_up(ask_bits_, best_ask_);
            }
        }
        return qty;
    }

    Quantity match_bids(OrderId taker, Quantity qty, std::int64_t limit_idx,
                        std::vector<Fill>& fills) {
        while (qty > 0 && best_bid_ >= 0 && best_bid_ >= limit_idx) {
            qty = consume_level(bid_levels_[best_bid_], taker, qty, min_price_ + best_bid_, fills);
            if (bid_levels_[best_bid_].head == NIL) {
                clear_bit(bid_bits_, best_bid_);
                best_bid_ = next_occupied_down(bid_bits_, best_bid_);
            }
        }
        return qty;
    }

    Quantity consume_level(Level& level, OrderId taker, Quantity qty, Price price,
                           std::vector<Fill>& fills) {
        while (qty > 0 && level.head != NIL) {
            Node& maker = pool_[level.head];
            Quantity traded = maker.qty < qty ? maker.qty : qty;
            fills.push_back({taker, maker.id, price, traded});
            qty -= traded;
            maker.qty -= traded;
            if (maker.qty == 0) {
                const std::uint32_t slot = level.head;
                index_.erase(maker.id);
                level.head = maker.next;
                if (level.head == NIL) level.tail = NIL;
                else pool_[level.head].prev = NIL;
                release(slot);
            }
        }
        return qty;
    }

    void rest(OrderId id, Side side, std::int64_t idx, Quantity qty) {
        const std::uint32_t slot = allocate();
        Level& level = (side == Side::Buy) ? bid_levels_[idx] : ask_levels_[idx];
        pool_[slot] = Node{id, qty, level.tail, NIL, idx, side};
        if (level.tail == NIL) level.head = slot;
        else pool_[level.tail].next = slot;
        level.tail = slot;
        index_.insert(id, slot);

        if (side == Side::Buy) {
            set_bit(bid_bits_, idx);
            if (idx > best_bid_) best_bid_ = idx;
        } else {
            set_bit(ask_bits_, idx);
            if (idx < best_ask_) best_ask_ = idx;
        }
    }

    void unlink_and_free(std::uint32_t slot) {
        Node& node = pool_[slot];
        Level& level = (node.side == Side::Buy) ? bid_levels_[node.ladder_idx]
                                                : ask_levels_[node.ladder_idx];
        if (node.prev == NIL) level.head = node.next;
        else pool_[node.prev].next = node.next;
        if (node.next == NIL) level.tail = node.prev;
        else pool_[node.next].prev = node.prev;

        // A cancel can empty any level; keep the bitmap (and, if it was the
        // best level, the best index) honest.
        if (level.head == NIL) {
            if (node.side == Side::Buy) {
                clear_bit(bid_bits_, node.ladder_idx);
                if (node.ladder_idx == best_bid_)
                    best_bid_ = next_occupied_down(bid_bits_, best_bid_);
            } else {
                clear_bit(ask_bits_, node.ladder_idx);
                if (node.ladder_idx == best_ask_)
                    best_ask_ = next_occupied_up(ask_bits_, best_ask_);
            }
        }
        release(slot);
    }

    // ---- occupancy bitmap: one bit per price level ----------------------

    static void set_bit(std::vector<std::uint64_t>& bits, std::int64_t idx) {
        bits[static_cast<std::size_t>(idx >> 6)] |= (1ull << (idx & 63));
    }

    static void clear_bit(std::vector<std::uint64_t>& bits, std::int64_t idx) {
        bits[static_cast<std::size_t>(idx >> 6)] &= ~(1ull << (idx & 63));
    }

    // Lowest occupied index >= from, or ladder_size_ if none: mask off the
    // bits below `from` in its word, then walk whole words. 64 price ticks
    // per iteration instead of one.
    std::int64_t next_occupied_up(const std::vector<std::uint64_t>& bits,
                                  std::int64_t from) const {
        std::size_t w = static_cast<std::size_t>(from >> 6);
        std::uint64_t word = bits[w] & (~0ull << (from & 63));
        while (true) {
            if (word != 0)
                return static_cast<std::int64_t>((w << 6) + std::countr_zero(word));
            if (++w >= bits.size()) return ladder_size_;
            word = bits[w];
        }
    }

    // Highest occupied index <= from, or -1 if none.
    std::int64_t next_occupied_down(const std::vector<std::uint64_t>& bits,
                                    std::int64_t from) const {
        if (from < 0) return -1;
        std::size_t w = static_cast<std::size_t>(from >> 6);
        std::uint64_t word = bits[w] & (~0ull >> (63 - (from & 63)));
        while (true) {
            if (word != 0)
                return static_cast<std::int64_t>((w << 6) + 63 - std::countl_zero(word));
            if (w == 0) return -1;
            word = bits[--w];
        }
    }

    std::uint32_t allocate() {
        if (!free_.empty()) {
            const std::uint32_t slot = free_.back();
            free_.pop_back();
            return slot;
        }
        pool_.emplace_back();
        return static_cast<std::uint32_t>(pool_.size() - 1);
    }

    void release(std::uint32_t slot) { free_.push_back(slot); }

    Price min_price_;
    std::int64_t ladder_size_;
    std::vector<Level> bid_levels_;
    std::vector<Level> ask_levels_;
    std::vector<std::uint64_t> bid_bits_;  // occupancy, one bit per level
    std::vector<std::uint64_t> ask_bits_;
    std::int64_t best_bid_;   // ladder index of best bid, -1 when none
    std::int64_t best_ask_;   // ladder index of best ask, ladder_size_ when none
    std::vector<Node> pool_;
    std::vector<std::uint32_t> free_;
    IdMap index_;
};

}  // namespace lob
