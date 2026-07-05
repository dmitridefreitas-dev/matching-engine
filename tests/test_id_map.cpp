// IdMap earns trust the same way the engines do: differentially, against
// the standard library, under randomized churn — plus targeted tests for
// the two things open addressing gets wrong when it's wrong (collision
// clusters and deletion).

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "lob/id_map.hpp"

using namespace lob;

TEST(IdMap, BasicInsertFindErase) {
    IdMap map(16);
    EXPECT_EQ(map.find(42), nullptr);
    map.insert(42, 7);
    ASSERT_NE(map.find(42), nullptr);
    EXPECT_EQ(*map.find(42), 7u);
    EXPECT_TRUE(map.erase(42));
    EXPECT_EQ(map.find(42), nullptr);
    EXPECT_FALSE(map.erase(42));
    EXPECT_EQ(map.size(), 0u);
}

TEST(IdMap, GrowsPastItsReserve) {
    IdMap map(4);  // tiny on purpose: force several rehashes
    for (OrderId id = 1; id <= 10'000; ++id) map.insert(id, static_cast<std::uint32_t>(id * 3));
    EXPECT_EQ(map.size(), 10'000u);
    for (OrderId id = 1; id <= 10'000; ++id) {
        ASSERT_NE(map.find(id), nullptr) << id;
        EXPECT_EQ(*map.find(id), static_cast<std::uint32_t>(id * 3));
    }
}

TEST(IdMap, BackwardShiftKeepsClustersReachable) {
    // Erase from the middle of a probe cluster; everything behind the hole
    // must still be findable (the classic tombstone-free deletion bug).
    IdMap map(8);
    for (OrderId id = 1; id <= 64; ++id) map.insert(id, static_cast<std::uint32_t>(id));
    for (OrderId id = 2; id <= 64; id += 2) EXPECT_TRUE(map.erase(id));
    for (OrderId id = 1; id <= 64; ++id) {
        if (id % 2 == 1) {
            ASSERT_NE(map.find(id), nullptr) << id;
            EXPECT_EQ(*map.find(id), static_cast<std::uint32_t>(id));
        } else {
            EXPECT_EQ(map.find(id), nullptr) << id;
        }
    }
}

TEST(IdMap, DifferentialAgainstUnorderedMap) {
    std::mt19937_64 rng(1234);
    IdMap ours(16);
    std::unordered_map<OrderId, std::uint32_t> reference;

    for (int step = 0; step < 200'000; ++step) {
        const OrderId id = 1 + rng() % 5'000;  // small key space -> heavy churn
        switch (rng() % 3) {
            case 0: {  // insert if absent
                if (!reference.count(id)) {
                    const auto value = static_cast<std::uint32_t>(rng());
                    reference.emplace(id, value);
                    ours.insert(id, value);
                }
                break;
            }
            case 1: {  // erase
                const bool expected = reference.erase(id) > 0;
                EXPECT_EQ(ours.erase(id), expected) << "step " << step;
                break;
            }
            case 2: {  // find
                auto it = reference.find(id);
                const std::uint32_t* got = ours.find(id);
                if (it == reference.end()) {
                    EXPECT_EQ(got, nullptr) << "step " << step;
                } else {
                    ASSERT_NE(got, nullptr) << "step " << step;
                    EXPECT_EQ(*got, it->second) << "step " << step;
                }
                break;
            }
        }
        if (step % 10'000 == 0) EXPECT_EQ(ours.size(), reference.size());
    }
    EXPECT_EQ(ours.size(), reference.size());
}
