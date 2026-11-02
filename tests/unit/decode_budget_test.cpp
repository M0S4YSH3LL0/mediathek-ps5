// Mediathek - Tests for the picture decoder's memory estimate and the thread count it allows.
// Copyright (C) 2026 BlackBearReloaded SPDX-License-Identifier: GPL-3.0-or-later

#include "media/decode_budget.hpp"

#include <gtest/gtest.h>

namespace
{

using namespace hui::media;

constexpr std::size_t kMiB = 1u << 20;

// Peak memory of FFmpeg decoding a 4K HEVC clip, measured on a PC with the
// converter's own share (about 90 MiB) taken off: 8-bit 102 / 130 / 162 / 212 MiB and
// 10-bit 175 / 215 / 265 / 320 MiB for 1 / 2 / 3 / 4 frame threads. Those are upper
// bounds: on the console the whole heap peaked at 238 MB with 35 MB in use before the
// film and about 55 MB of queues, so one 10-bit thread took at most 148 MB there.
// The estimate has to stay above what the console showed and above the PC's growth per
// thread (a console that runs out of heap crashes), and not wildly above the PC's totals
// (a film that would have fitted is then played on fewer threads).
TEST(DecodeBudget, FourKEstimatesCoverWhatWasMeasured)
{
    EXPECT_GE(decoder_bytes(3840, 2160, 2, 1), 148u * 1000 * 1000) << "console, 10 bit";
    EXPECT_GE(decoder_bytes(3840, 2160, 2, 4) - decoder_bytes(3840, 2160, 2, 1), 145 * kMiB);
    EXPECT_GE(decoder_bytes(3840, 2160, 1, 4) - decoder_bytes(3840, 2160, 1, 1), 108 * kMiB);

    const std::size_t eight[] = {102, 130, 162, 212};
    const std::size_t ten[] = {175, 215, 265, 320};
    for (int threads = 1; threads <= 4; ++threads)
    {
        const std::size_t i = static_cast<std::size_t>(threads - 1);
        EXPECT_LE(decoder_bytes(3840, 2160, 1, threads), eight[i] * kMiB * 13 / 10)
            << threads << " threads, 8 bit";
        EXPECT_LE(decoder_bytes(3840, 2160, 2, threads), ten[i] * kMiB * 13 / 10)
            << threads << " threads, 10 bit";
    }
}

// The room the console reported for a 4K 10-bit film (248512856 bytes) used to be refused
// two threads by an estimate that was too cautious; two fit, with 50 MB to spare.
TEST(DecodeBudget, TheConsolesRoomGivesAFourKTenBitFilmTwoThreads)
{
    EXPECT_EQ(decoder_threads(3840, 2160, 2, 4, 248512856), 2);
    EXPECT_EQ(decoder_threads(3840, 2160, 1, 4, 248512856), 4);
}

TEST(DecodeBudget, MoreThreadsAndBiggerPicturesCostMore)
{
    EXPECT_LT(decoder_bytes(3840, 2160, 2, 1), decoder_bytes(3840, 2160, 2, 4));
    EXPECT_LT(decoder_bytes(1920, 1080, 1, 4), decoder_bytes(3840, 2160, 1, 4));
    EXPECT_LT(decoder_bytes(3840, 2160, 1, 4), decoder_bytes(3840, 2160, 2, 4));
}

TEST(DecodeBudget, ThePicturesOfAnOrdinaryFilmKeepAllTheThreadsTheyAskFor)
{
    EXPECT_EQ(decoder_threads(1920, 1080, 1, 4, 100 * kMiB), 4);
    EXPECT_EQ(decoder_threads(1280, 720, 1, 4, 100 * kMiB), 4);
}

// The budget of a 384 MiB heap after the app, the pictures and the packet queue
// (about 245 MiB with the queue cut to 32 MiB): 8-bit 4K may use every thread,
// 10-bit 4K must give some up, or the heap runs out and the app dies.
TEST(DecodeBudget, FourKTenBitGivesUpThreadsWhereEightBitKeepsThem)
{
    const std::size_t room = 245 * kMiB;
    EXPECT_EQ(decoder_threads(3840, 2160, 1, 4, room), 4);
    const int ten = decoder_threads(3840, 2160, 2, 4, room);
    EXPECT_GE(ten, 1);
    EXPECT_LT(ten, 4);
    EXPECT_LE(decoder_bytes(3840, 2160, 2, ten), room);
}

TEST(DecodeBudget, TheChosenThreadsAlwaysFitAndOneMoreWouldNot)
{
    for (std::size_t room = 120; room <= 400; room += 20)
    {
        const int threads = decoder_threads(3840, 2160, 2, 4, room * kMiB);
        if (threads > 0)
            EXPECT_LE(decoder_bytes(3840, 2160, 2, threads), room * kMiB) << room;
        if (threads > 0 && threads < 4)
            EXPECT_GT(decoder_bytes(3840, 2160, 2, threads + 1), room * kMiB) << room;
    }
}

TEST(DecodeBudget, AFilmThatCannotFitEvenOneThreadIsRefused)
{
    EXPECT_EQ(decoder_threads(3840, 2160, 2, 4, 64 * kMiB), 0);
    EXPECT_EQ(decoder_threads(3840, 2160, 2, 4, 0), 0);
}

TEST(DecodeBudget, NonsenseSizesCostNothingAndNeverAskForNegativeThreads)
{
    EXPECT_EQ(decoder_bytes(0, 0, 1, 4), 0u);
    EXPECT_EQ(decoder_bytes(-5, 100, 1, 4), 0u);
    EXPECT_EQ(decoder_threads(0, 0, 1, 4, 10 * kMiB), 4);
    EXPECT_EQ(decoder_threads(1920, 1080, 1, 0, 100 * kMiB), 1);
}

} // namespace
