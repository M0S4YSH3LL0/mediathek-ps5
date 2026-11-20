// Mediathek - Tests for the blocks of memory decoded pictures are kept in.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/picture_pool.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>

namespace
{

using hui::media::PicturePool;

// Counts what the pool asks of the system, which is what costs on the console.
struct System
{
    std::map<void *, std::size_t> live;
    int allocations = 0;
    std::size_t refuse_above = 0; // 0: give whatever is asked

    PicturePool::Backend backend()
    {
        PicturePool::Backend backend;
        backend.allocate = [this](std::size_t bytes) -> void *
        {
            if (refuse_above != 0 && bytes > refuse_above)
                return nullptr;
            void *address = std::malloc(bytes);
            live[address] = bytes;
            ++allocations;
            return address;
        };
        backend.release = [this](void *address, std::size_t bytes)
        {
            EXPECT_EQ(live[address], bytes);
            live.erase(address);
            std::free(address);
        };
        return backend;
    }
};

TEST(PicturePool, ABlockGivenBackIsHandedOutAgainWithoutAskingTheSystem)
{
    System system;
    PicturePool pool(system.backend(), 8);
    void *first = pool.take(1000);
    ASSERT_NE(first, nullptr);
    pool.give(first);
    EXPECT_EQ(pool.take(1000), first);
    EXPECT_EQ(system.allocations, 1);
    EXPECT_EQ(pool.in_use(), 1u);
    pool.give(first);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(PicturePool, TheSmallestFreeBlockThatFitsIsTheOneTaken)
{
    System system;
    PicturePool pool(system.backend(), 8);
    void *big = pool.take(4000);
    void *small = pool.take(1000);
    pool.give(big);
    pool.give(small);
    EXPECT_EQ(pool.take(900), small);
    EXPECT_EQ(pool.take(900), big) << "the small one is in use, the big one still fits";
    EXPECT_EQ(system.allocations, 2);
}

TEST(PicturePool, BlocksInUseAreNeverHandedOutTwice)
{
    System system;
    PicturePool pool(system.backend(), 8);
    void *a = pool.take(100);
    void *b = pool.take(100);
    EXPECT_NE(a, b);
    EXPECT_EQ(pool.blocks(), 2u);
    EXPECT_EQ(pool.bytes(), 200u);
}

TEST(PicturePool, WhenFullABiggerSizeReplacesAFreeSmallerBlock)
{
    System system;
    PicturePool pool(system.backend(), 2);
    void *a = pool.take(100);
    void *b = pool.take(100);
    EXPECT_EQ(pool.take(100), nullptr) << "both in use and no room for a third";
    pool.give(a);
    void *bigger = pool.take(500); // a film that changed its picture size
    ASSERT_NE(bigger, nullptr);
    EXPECT_EQ(pool.blocks(), 2u);
    EXPECT_EQ(pool.bytes(), 600u);
    EXPECT_EQ(system.live.size(), 2u) << "the free small block went back to the system";
    pool.give(b);
    pool.give(bigger);
}

TEST(PicturePool, NothingWhenTheSystemHasNoMore)
{
    System system;
    system.refuse_above = 1000;
    PicturePool pool(system.backend(), 8);
    EXPECT_EQ(pool.take(5000), nullptr);
    EXPECT_EQ(pool.blocks(), 0u);
}

TEST(PicturePool, EveryBlockGoesBackToTheSystemWithThePool)
{
    System system;
    {
        PicturePool pool(system.backend(), 8);
        pool.give(pool.take(100));
        pool.give(pool.take(300));
        pool.give(pool.take(200));
    }
    EXPECT_TRUE(system.live.empty());
}

} // namespace
