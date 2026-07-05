// Open-addressed order-id -> pool-slot map.
//
// The profiler's answer to "where does FastBook's time go" was: the
// std::unordered_map from order id to pool slot, touched by every submit,
// fill, and cancel. Its per-node allocation and pointer-chasing buckets are
// exactly the costs the rest of the engine was designed away from. This
// replaces it with the standard production shape:
//
//   * one flat array of {key, value} cells, power-of-two sized
//   * linear probing (the next probe is the next cache line, not a pointer)
//   * multiplicative (Fibonacci) hashing to spread sequential ids
//   * BACKWARD-SHIFT deletion instead of tombstones: on erase, subsequent
//     cells are shifted back into the hole while doing so shortens their
//     probe distance — the table is always tombstone-free, so lookups never
//     slow down as churn accumulates (and order books are nothing but churn)
//
// Contract: keys are nonzero (0 is the empty sentinel — the engine asserts
// ids != 0), unique until erased. Grows at 50% load; with the constructor
// reserve, growth never happens in steady state.
//
// Correctness is established the same way as the engines': a randomized
// differential test against std::unordered_map in tests/test_id_map.cpp.

#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

class IdMap {
public:
    explicit IdMap(std::size_t expected = 1 << 20) {
        std::size_t capacity = 16;
        while (capacity < expected * 2) capacity <<= 1;
        cells_.assign(capacity, Cell{0, 0});
        mask_ = capacity - 1;
    }

    void insert(OrderId id, std::uint32_t value) {
        assert(id != 0);
        if ((count_ + 1) * 2 > cells_.size()) grow();
        std::size_t i = index_of(id);
        while (cells_[i].key != 0) {
            assert(cells_[i].key != id);  // duplicate insert is a caller bug
            i = (i + 1) & mask_;
        }
        cells_[i] = Cell{id, value};
        ++count_;
    }

    // Pointer to the value, or nullptr. Stable only until the next mutation.
    const std::uint32_t* find(OrderId id) const {
        std::size_t i = index_of(id);
        while (cells_[i].key != 0) {
            if (cells_[i].key == id) return &cells_[i].value;
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    bool erase(OrderId id) {
        std::size_t i = index_of(id);
        while (cells_[i].key != id) {
            if (cells_[i].key == 0) return false;
            i = (i + 1) & mask_;
        }
        // Backward-shift: pull later cells into the hole whenever that
        // moves them closer to (or onto) their ideal position.
        std::size_t hole = i;
        std::size_t probe = (i + 1) & mask_;
        while (cells_[probe].key != 0) {
            const std::size_t ideal = index_of(cells_[probe].key);
            if (((probe - ideal) & mask_) >= ((probe - hole) & mask_)) {
                cells_[hole] = cells_[probe];
                hole = probe;
            }
            probe = (probe + 1) & mask_;
        }
        cells_[hole].key = 0;
        --count_;
        return true;
    }

    std::size_t size() const { return count_; }

private:
    struct Cell {
        OrderId key;
        std::uint32_t value;
    };

    std::size_t index_of(OrderId id) const {
        // Fibonacci hashing: sequential ids (the common case — exchanges
        // hand them out in order) land far apart instead of clustering.
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> 32) & mask_;
    }

    void grow() {
        std::vector<Cell> old = std::move(cells_);
        cells_.assign(old.size() * 2, Cell{0, 0});
        mask_ = cells_.size() - 1;
        count_ = 0;
        for (const Cell& cell : old)
            if (cell.key != 0) insert(cell.key, cell.value);
    }

    std::vector<Cell> cells_;
    std::size_t mask_ = 0;
    std::size_t count_ = 0;
};

}  // namespace lob
