// Synthetic order-flow generator, shared by the differential fuzzer and the
// benchmarks.
//
// Determinism matters more than statistical elegance here: the same seed
// must produce byte-identical streams on every platform and standard
// library, so all randomness is raw mt19937_64 output reduced by modulo —
// std::uniform_int_distribution is implementation-defined and would break
// cross-platform reproducibility.
//
// The flow is realistic in shape, not calibrated: a random-walking mid,
// passive orders spread over ~12 ticks around it, a 25% aggressor fraction
// that crosses the spread, heavy-tailed sizes, and a submit/cancel/reduce/
// market mix of roughly 55/25/10/10 — matching the fact that most real
// order-book traffic is submissions and cancellations, not trades.

#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "lob/types.hpp"

namespace lob {

struct FlowOp {
    enum class Kind : std::uint8_t { Limit, Market, Cancel, Reduce };
    Kind kind;
    OrderId id;        // order id for Limit/Market, target id for Cancel/Reduce
    Side side;         // Limit/Market only
    Price price;       // Limit only
    Quantity quantity; // Limit/Market/Reduce (Reduce: the new, lower quantity)
};

struct FlowConfig {
    std::size_t operations = 100'000;
    std::uint64_t seed = 42;
    Price min_price = 100;
    Price max_price = 20'000;
    Price start_mid = 10'000;
};

inline std::vector<FlowOp> generate_flow(const FlowConfig& cfg) {
    std::mt19937_64 rng(cfg.seed);
    auto roll = [&rng](std::uint64_t n) { return rng() % n; };

    std::vector<FlowOp> ops;
    ops.reserve(cfg.operations);
    std::vector<OrderId> issued;  // ids ever submitted; stale targets are fine
    issued.reserve(cfg.operations);

    Price mid = cfg.start_mid;
    OrderId next_id = 1;

    for (std::size_t i = 0; i < cfg.operations; ++i) {
        // Mid drifts one tick 30% of the time, clamped inside the ladder.
        if (roll(10) < 3) {
            mid += (roll(2) == 0) ? 1 : -1;
            if (mid < cfg.min_price + 16) mid = cfg.min_price + 16;
            if (mid > cfg.max_price - 16) mid = cfg.max_price - 16;
        }

        const std::uint64_t dice = roll(100);
        if (dice < 55 || issued.empty()) {  // new limit order
            const Side side = (roll(2) == 0) ? Side::Buy : Side::Sell;
            const bool aggressive = roll(100) < 25;
            const Price offset = static_cast<Price>(1 + roll(12));
            Price price;
            if (aggressive) {  // cross the spread by a few ticks
                const Price through = static_cast<Price>(roll(3));
                price = (side == Side::Buy) ? mid + 1 + through : mid - 1 - through;
            } else {  // rest away from the mid
                price = (side == Side::Buy) ? mid - offset : mid + offset;
            }
            // Heavy-tailed sizes: mostly small lots, occasional blocks.
            const Quantity qty = (roll(10) == 0)
                                     ? static_cast<Quantity>(100 + roll(900))
                                     : static_cast<Quantity>(1 + roll(99));
            ops.push_back({FlowOp::Kind::Limit, next_id, side, price, qty});
            issued.push_back(next_id++);
        } else if (dice < 65) {  // market order
            const Side side = (roll(2) == 0) ? Side::Buy : Side::Sell;
            const Quantity qty = static_cast<Quantity>(1 + roll(199));
            ops.push_back({FlowOp::Kind::Market, next_id++, side, Price{0}, qty});
        } else if (dice < 90) {  // cancel a (possibly already gone) past order
            const OrderId target = issued[roll(issued.size())];
            ops.push_back({FlowOp::Kind::Cancel, target, Side::Buy, Price{0}, Quantity{0}});
        } else {  // reduce a past order to a smaller size
            const OrderId target = issued[roll(issued.size())];
            const Quantity new_qty = static_cast<Quantity>(1 + roll(49));
            ops.push_back({FlowOp::Kind::Reduce, target, Side::Buy, Price{0}, new_qty});
        }
    }
    return ops;
}

// Drive one operation into any book; returns fills via `fills`.
template <typename Book>
inline void apply(Book& book, const FlowOp& op, std::vector<Fill>& fills) {
    switch (op.kind) {
        case FlowOp::Kind::Limit:
            book.submit_limit(op.id, op.side, op.price, op.quantity, fills);
            break;
        case FlowOp::Kind::Market:
            book.submit_market(op.id, op.side, op.quantity, fills);
            break;
        case FlowOp::Kind::Cancel:
            book.cancel(op.id);
            break;
        case FlowOp::Kind::Reduce:
            book.reduce(op.id, op.quantity);
            break;
    }
}

}  // namespace lob
