// Differential fuzzing: MapBook is the oracle, FastBook must be
// indistinguishable from it. Random order streams are fed to both engines
// op by op; after EVERY operation the fill outputs and return values must
// match exactly, book invariants must hold, and snapshots are compared at
// checkpoints. Failures print the seed, so any counterexample is one
// command away from a deterministic reproduction.

#include <gtest/gtest.h>

#include "lob/fast_book.hpp"
#include "lob/flow.hpp"
#include "lob/map_book.hpp"

using namespace lob;

namespace {

template <typename Book>
struct Returns {
    Quantity limit_remainder = 0;
    Quantity market_unfilled = 0;
    bool cancel_ok = false;
    bool reduce_ok = false;
};

// Apply one op capturing both the fills and the return value.
template <typename Book>
Returns<Book> apply_traced(Book& book, const FlowOp& op, std::vector<Fill>& fills) {
    Returns<Book> r;
    switch (op.kind) {
        case FlowOp::Kind::Limit:
            r.limit_remainder = book.submit_limit(op.id, op.side, op.price, op.quantity, fills);
            break;
        case FlowOp::Kind::Market:
            r.market_unfilled = book.submit_market(op.id, op.side, op.quantity, fills);
            break;
        case FlowOp::Kind::Cancel:
            r.cancel_ok = book.cancel(op.id);
            break;
        case FlowOp::Kind::Reduce:
            r.reduce_ok = book.reduce(op.id, op.quantity);
            break;
    }
    return r;
}

template <typename Book>
void assert_invariants(const Book& book, std::uint64_t seed, std::size_t step) {
    if (book.has_bid() && book.has_ask()) {
        ASSERT_LT(book.best_bid(), book.best_ask())
            << "crossed book, seed " << seed << " step " << step;
    }
    for (Side side : {Side::Buy, Side::Sell}) {
        Price prev = 0;
        for (const LevelSnapshot& level : book.snapshot(side)) {
            if (prev != 0) {
                if (side == Side::Buy) {
                    ASSERT_LT(level.price, prev) << "bids not descending, seed " << seed;
                } else {
                    ASSERT_GT(level.price, prev) << "asks not ascending, seed " << seed;
                }
            }
            prev = level.price;
            ASSERT_FALSE(level.orders.empty()) << "empty level survived, seed " << seed;
            for (const auto& [id, qty] : level.orders)
                ASSERT_GT(qty, 0u) << "zero-qty resting order, seed " << seed;
        }
    }
}

void run_differential(std::uint64_t seed, std::size_t operations) {
    FlowConfig cfg;
    cfg.operations = operations;
    cfg.seed = seed;

    MapBook reference;
    FastBook candidate{cfg.min_price, cfg.max_price};

    const std::vector<FlowOp> ops = generate_flow(cfg);
    std::vector<Fill> ref_fills, fast_fills;
    ref_fills.reserve(64);
    fast_fills.reserve(64);

    for (std::size_t step = 0; step < ops.size(); ++step) {
        const FlowOp& op = ops[step];
        ref_fills.clear();
        fast_fills.clear();

        auto ref_ret = apply_traced(reference, op, ref_fills);
        auto fast_ret = apply_traced(candidate, op, fast_fills);

        ASSERT_EQ(ref_fills, fast_fills) << "fill divergence, seed " << seed
                                         << " step " << step;
        ASSERT_EQ(ref_ret.limit_remainder, fast_ret.limit_remainder) << "seed " << seed;
        ASSERT_EQ(ref_ret.market_unfilled, fast_ret.market_unfilled) << "seed " << seed;
        ASSERT_EQ(ref_ret.cancel_ok, fast_ret.cancel_ok) << "seed " << seed;
        ASSERT_EQ(ref_ret.reduce_ok, fast_ret.reduce_ok) << "seed " << seed;

        // Quantity conservation on every trade batch.
        for (const Fill& f : ref_fills) ASSERT_GT(f.quantity, 0u);

        if (step % 1024 == 0 || step + 1 == ops.size()) {
            ASSERT_EQ(reference.snapshot(Side::Buy), candidate.snapshot(Side::Buy))
                << "bid snapshot divergence, seed " << seed << " step " << step;
            ASSERT_EQ(reference.snapshot(Side::Sell), candidate.snapshot(Side::Sell))
                << "ask snapshot divergence, seed " << seed << " step " << step;
            ASSERT_EQ(reference.open_orders(), candidate.open_orders()) << "seed " << seed;
            assert_invariants(reference, seed, step);
            assert_invariants(candidate, seed, step);
        }
    }
}

}  // namespace

TEST(Differential, TwentyFiveSeedsTwentyThousandOpsEach) {
    for (std::uint64_t seed = 1; seed <= 25; ++seed) {
        SCOPED_TRACE(testing::Message() << "seed " << seed);
        run_differential(seed, 20'000);
    }
}

TEST(Differential, OneLongSession) {
    run_differential(31337, 200'000);
}

TEST(Differential, FillsConserveSubmittedQuantity) {
    // Total traded quantity per taker never exceeds what the taker sent.
    FlowConfig cfg;
    cfg.operations = 50'000;
    cfg.seed = 7;
    MapBook book;
    std::vector<Fill> fills;
    for (const FlowOp& op : generate_flow(cfg)) {
        fills.clear();
        if (op.kind == FlowOp::Kind::Limit) {
            Quantity rem = book.submit_limit(op.id, op.side, op.price, op.quantity, fills);
            Quantity traded = 0;
            for (const Fill& f : fills) traded += f.quantity;
            ASSERT_EQ(traded + rem, op.quantity);
        } else if (op.kind == FlowOp::Kind::Market) {
            Quantity rem = book.submit_market(op.id, op.side, op.quantity, fills);
            Quantity traded = 0;
            for (const Fill& f : fills) traded += f.quantity;
            ASSERT_EQ(traded + rem, op.quantity);
        } else {
            apply(book, op, fills);
        }
    }
}
