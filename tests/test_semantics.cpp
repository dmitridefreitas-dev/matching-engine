// The semantics contract, run against BOTH implementations via typed tests.
// One suite, two engines: if FastBook ever diverges from MapBook on any of
// these behaviours, the same test names fail for exactly one of the two.

#include <gtest/gtest.h>

#include "lob/fast_book.hpp"
#include "lob/map_book.hpp"

using namespace lob;

template <typename BookT>
BookT make_book();

template <>
MapBook make_book<MapBook>() { return MapBook{}; }

template <>
FastBook make_book<FastBook>() { return FastBook{1, 100'000}; }

template <typename BookT>
class BookSemantics : public ::testing::Test {
protected:
    BookT book = make_book<BookT>();
    std::vector<Fill> fills;
};

using Implementations = ::testing::Types<MapBook, FastBook>;
TYPED_TEST_SUITE(BookSemantics, Implementations);

TYPED_TEST(BookSemantics, MarketOrderOnEmptyBookFillsNothing) {
    Quantity unfilled = this->book.submit_market(1, Side::Buy, 50, this->fills);
    EXPECT_EQ(unfilled, 50u);
    EXPECT_TRUE(this->fills.empty());
    EXPECT_FALSE(this->book.has_ask());
    EXPECT_FALSE(this->book.has_bid());
}

TYPED_TEST(BookSemantics, RestingOrderSitsAtItsPrice) {
    Quantity remainder = this->book.submit_limit(1, Side::Buy, 100, 10, this->fills);
    EXPECT_EQ(remainder, 10u);
    EXPECT_TRUE(this->fills.empty());
    EXPECT_TRUE(this->book.has_bid());
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->book.open_orders(), 1u);
}

TYPED_TEST(BookSemantics, CrossingLimitFillsAtMakerPrice) {
    this->book.submit_limit(1, Side::Sell, 100, 10, this->fills);
    // Buyer willing to pay 105 trades at the resting 100: taker improvement.
    Quantity remainder = this->book.submit_limit(2, Side::Buy, 105, 10, this->fills);
    EXPECT_EQ(remainder, 0u);
    ASSERT_EQ(this->fills.size(), 1u);
    EXPECT_EQ(this->fills[0], (Fill{2, 1, 100, 10}));
    EXPECT_FALSE(this->book.has_ask());
    EXPECT_FALSE(this->book.has_bid());
}

TYPED_TEST(BookSemantics, PartialFillRestsTheRemainder) {
    this->book.submit_limit(1, Side::Sell, 100, 4, this->fills);
    Quantity remainder = this->book.submit_limit(2, Side::Buy, 100, 10, this->fills);
    EXPECT_EQ(remainder, 6u);
    ASSERT_EQ(this->fills.size(), 1u);
    EXPECT_EQ(this->fills[0], (Fill{2, 1, 100, 4}));
    EXPECT_TRUE(this->book.has_bid());
    EXPECT_EQ(this->book.best_bid(), 100);
    auto bids = this->book.snapshot(Side::Buy);
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids[0].orders[0].first, 2u);
    EXPECT_EQ(bids[0].orders[0].second, 6u);
}

TYPED_TEST(BookSemantics, FifoWithinALevel) {
    this->book.submit_limit(1, Side::Sell, 100, 5, this->fills);
    this->book.submit_limit(2, Side::Sell, 100, 5, this->fills);
    this->book.submit_limit(3, Side::Buy, 100, 7, this->fills);
    ASSERT_EQ(this->fills.size(), 2u);
    EXPECT_EQ(this->fills[0], (Fill{3, 1, 100, 5}));  // first in fills first
    EXPECT_EQ(this->fills[1], (Fill{3, 2, 100, 2}));
    auto asks = this->book.snapshot(Side::Sell);
    ASSERT_EQ(asks.size(), 1u);
    EXPECT_EQ(asks[0].orders[0].first, 2u);
    EXPECT_EQ(asks[0].orders[0].second, 3u);
}

TYPED_TEST(BookSemantics, PricePriorityBeatsTimePriority) {
    this->book.submit_limit(1, Side::Sell, 101, 5, this->fills);  // earlier, worse
    this->book.submit_limit(2, Side::Sell, 100, 5, this->fills);  // later, better
    this->book.submit_market(3, Side::Buy, 5, this->fills);
    ASSERT_EQ(this->fills.size(), 1u);
    EXPECT_EQ(this->fills[0], (Fill{3, 2, 100, 5}));
    EXPECT_EQ(this->book.best_ask(), 101);
}

TYPED_TEST(BookSemantics, MarketOrderWalksTheBook) {
    this->book.submit_limit(1, Side::Sell, 100, 5, this->fills);
    this->book.submit_limit(2, Side::Sell, 101, 5, this->fills);
    Quantity unfilled = this->book.submit_market(3, Side::Buy, 12, this->fills);
    EXPECT_EQ(unfilled, 2u);  // the remainder is discarded, never rests
    ASSERT_EQ(this->fills.size(), 2u);
    EXPECT_EQ(this->fills[0], (Fill{3, 1, 100, 5}));
    EXPECT_EQ(this->fills[1], (Fill{3, 2, 101, 5}));
    EXPECT_FALSE(this->book.has_ask());
    EXPECT_FALSE(this->book.has_bid());
}

TYPED_TEST(BookSemantics, NonCrossingLimitsDoNotTrade) {
    this->book.submit_limit(1, Side::Buy, 99, 10, this->fills);
    this->book.submit_limit(2, Side::Sell, 101, 10, this->fills);
    EXPECT_TRUE(this->fills.empty());
    EXPECT_EQ(this->book.best_bid(), 99);
    EXPECT_EQ(this->book.best_ask(), 101);
}

TYPED_TEST(BookSemantics, CancelRemovesARestingOrder) {
    this->book.submit_limit(1, Side::Buy, 100, 10, this->fills);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_FALSE(this->book.has_bid());
    EXPECT_EQ(this->book.open_orders(), 0u);
    EXPECT_FALSE(this->book.cancel(1));   // already gone
    EXPECT_FALSE(this->book.cancel(99));  // never existed
}

TYPED_TEST(BookSemantics, CancelledOrderNeverFills) {
    this->book.submit_limit(1, Side::Sell, 100, 5, this->fills);
    this->book.submit_limit(2, Side::Sell, 100, 5, this->fills);
    this->book.cancel(1);
    this->book.submit_market(3, Side::Buy, 5, this->fills);
    ASSERT_EQ(this->fills.size(), 1u);
    EXPECT_EQ(this->fills[0].maker, 2u);
}

TYPED_TEST(BookSemantics, ReduceKeepsTimePriority) {
    this->book.submit_limit(1, Side::Sell, 100, 10, this->fills);
    this->book.submit_limit(2, Side::Sell, 100, 10, this->fills);
    EXPECT_TRUE(this->book.reduce(1, 3));  // size down in place
    this->book.submit_market(3, Side::Buy, 5, this->fills);
    ASSERT_EQ(this->fills.size(), 2u);
    EXPECT_EQ(this->fills[0], (Fill{3, 1, 100, 3}));  // order 1 kept its place
    EXPECT_EQ(this->fills[1], (Fill{3, 2, 100, 2}));
}

TYPED_TEST(BookSemantics, ReduceRules) {
    this->book.submit_limit(1, Side::Buy, 100, 10, this->fills);
    EXPECT_FALSE(this->book.reduce(1, 10));  // not a reduction
    EXPECT_FALSE(this->book.reduce(1, 15));  // size up needs cancel+replace
    EXPECT_FALSE(this->book.reduce(42, 5));  // unknown id
    EXPECT_TRUE(this->book.reduce(1, 0));    // reduce-to-zero acts as cancel
    EXPECT_FALSE(this->book.has_bid());
}

TYPED_TEST(BookSemantics, AggressiveLimitRestsAfterClearingTheBook) {
    this->book.submit_limit(1, Side::Sell, 100, 5, this->fills);
    Quantity remainder = this->book.submit_limit(2, Side::Buy, 102, 12, this->fills);
    EXPECT_EQ(remainder, 7u);
    EXPECT_TRUE(this->book.has_bid());
    EXPECT_EQ(this->book.best_bid(), 102);  // leftover rests at its own limit
}

TEST(FastBookSpecific, PriceOutsideLadderThrows) {
    FastBook book{100, 200};
    std::vector<Fill> fills;
    EXPECT_THROW(book.submit_limit(1, Side::Buy, 99, 10, fills), std::out_of_range);
    EXPECT_THROW(book.submit_limit(2, Side::Buy, 201, 10, fills), std::out_of_range);
    EXPECT_THROW(FastBook(0, 10), std::invalid_argument);
    EXPECT_THROW(FastBook(10, 9), std::invalid_argument);
}
