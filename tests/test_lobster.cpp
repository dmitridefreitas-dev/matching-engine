// LOBSTER parser against a hand-written 8-row fixture (our own file in
// LOBSTER's format — not LOBSTER data). Every mapping rule is exercised:
// submits, a partial cancel that becomes reduce-to, a delete, a visible
// execution that synthesises the opposite-side aggressor, and a hidden
// execution that is skipped.

#include <gtest/gtest.h>

#include <string>

#include "lob/fast_book.hpp"
#include "lob/lobster.hpp"
#include "lob/map_book.hpp"

using namespace lob;

namespace {
std::string fixture(const char* name) {
    return std::string(LOB_FIXTURE_DIR) + "/" + name;
}
}  // namespace

TEST(Lobster, ParsesTheFixtureWithEveryRuleApplied) {
    LobsterStats stats;
    auto ops = load_lobster(fixture("mini_lobster.csv"), stats);

    EXPECT_EQ(stats.submits, 4u);                  // rows with type 1
    EXPECT_EQ(stats.reduces, 1u);                  // type 2
    EXPECT_EQ(stats.cancels, 1u);                  // type 3
    EXPECT_EQ(stats.synthesized_executions, 1u);   // type 4
    EXPECT_EQ(stats.skipped, 1u);                  // type 5 (hidden)
    ASSERT_EQ(ops.size(), 7u);

    // Row 4: partial cancel of 40 from order 11 (size 100) -> reduce to 60.
    EXPECT_EQ(ops[3].kind, FlowOp::Kind::Reduce);
    EXPECT_EQ(ops[3].id, 11u);
    EXPECT_EQ(ops[3].quantity, 60u);

    // Row 5: visible execution of sell order 12 -> synthetic BUY aggressor
    // at the printed price and size.
    EXPECT_EQ(ops[4].kind, FlowOp::Kind::Limit);
    EXPECT_EQ(ops[4].side, Side::Buy);
    EXPECT_EQ(ops[4].price, 2239500);
    EXPECT_EQ(ops[4].quantity, 50u);
    EXPECT_GE(ops[4].id, 1'000'000'000'000ULL);

    EXPECT_EQ(stats.min_price, 2238500);
    EXPECT_EQ(stats.max_price, 2240000);
}

TEST(Lobster, ReplayIsDifferentiallyConsistent) {
    LobsterStats stats;
    auto ops = load_lobster(fixture("mini_lobster.csv"), stats);

    MapBook reference;
    FastBook candidate{2200000, 2300000};
    std::vector<Fill> ref_fills, fast_fills;
    for (const FlowOp& op : ops) {
        apply(reference, op, ref_fills);
        apply(candidate, op, fast_fills);
    }
    EXPECT_EQ(ref_fills, fast_fills);
    EXPECT_EQ(reference.snapshot(Side::Buy), candidate.snapshot(Side::Buy));
    EXPECT_EQ(reference.snapshot(Side::Sell), candidate.snapshot(Side::Sell));
    // The synthetic aggressor from row 5 lifts resting sell order 12 exactly.
    ASSERT_FALSE(ref_fills.empty());
    EXPECT_EQ(ref_fills[0].maker, 12u);
    EXPECT_EQ(ref_fills[0].quantity, 50u);
}

TEST(Lobster, MissingFileThrows) {
    LobsterStats stats;
    EXPECT_THROW(load_lobster(fixture("does_not_exist.csv"), stats), std::runtime_error);
}
