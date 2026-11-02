// Mediathek - Tests for how the player drops frames when the decoder cannot keep up.
// Copyright (C) 2026 BlackBearReloaded SPDX-License-Identifier: GPL-3.0-or-later

#include "media/decode_pace.hpp"

#include <gtest/gtest.h>

namespace
{

using hui::media::DecodePace;
using hui::media::Skip;

// Lag is how far the packet is behind the sound, in seconds.
TEST(DecodePace, ADecoderThatKeepsUpDropsNothing)
{
    DecodePace pace;
    for (double lag : {-0.5, 0.0, 0.1, 0.2})
        EXPECT_EQ(pace.next(lag, false), Skip::none) << lag;
}

// The player stops showing pictures that are 0.3 s late, so thinning has to start before that.
TEST(DecodePace, ThinningStartsBeforeLatePicturesAreDropped)
{
    DecodePace pace;
    EXPECT_EQ(pace.next(0.26, false), Skip::non_reference);
}

TEST(DecodePace, FallingBehindDropsFramesNothingElseIsPredictedFromAndKeepsDoingSo)
{
    DecodePace pace;
    EXPECT_EQ(pace.next(0.5, false), Skip::non_reference);
    // Back in step, but a decoder that was too slow once still is: no flapping.
    EXPECT_EQ(pace.next(0.0, false), Skip::non_reference);
    EXPECT_EQ(pace.next(-1.0, true), Skip::non_reference);
}

// A seek lands up to a key-frame interval before its target. Skipping from there would
// hold the old picture until the key frame after the target, with the sound running.
TEST(DecodePace, TheWayFromAKeyFrameToASeekTargetIsNotLag)
{
    DecodePace pace;
    EXPECT_EQ(pace.next(5.0, true, false), Skip::none);
    EXPECT_EQ(pace.next(4.98, false, false), Skip::none);
    EXPECT_EQ(pace.next(0.01, false, false), Skip::none);
    // Having been told nothing about the way there, the first picture starts clean.
    EXPECT_EQ(pace.next(0.02, false, true), Skip::none);
}

TEST(DecodePace, OnceThePictureHasArrivedFallingBehindStillJumps)
{
    DecodePace pace;
    EXPECT_EQ(pace.next(5.0, false, false), Skip::none);
    EXPECT_EQ(pace.next(1.0, false, true), Skip::to_key_frame);
}

TEST(DecodePace, ALongWayBehindJumpsToTheNextKeyFrameNearTheSound)
{
    DecodePace pace;
    EXPECT_EQ(pace.next(4.0, false), Skip::to_key_frame);
    EXPECT_EQ(pace.next(3.5, false), Skip::to_key_frame);
    // A key frame that is still old does not end the jump, nor does a late one that is no key.
    EXPECT_EQ(pace.next(2.0, true), Skip::to_key_frame);
    EXPECT_EQ(pace.next(0.1, false), Skip::to_key_frame);
    // The first key frame close to the sound does, and the frames after it are thinned.
    EXPECT_EQ(pace.next(0.1, true), Skip::non_reference);
    EXPECT_EQ(pace.next(0.0, false), Skip::non_reference);
}

// Nothing cheaper than thinning exists that keeps the picture right (a skipped
// loop filter smears into every frame predicted from it), so a decoder that is lost
// again just jumps again.
TEST(DecodePace, ADecoderThatIsLostAgainJumpsAgain)
{
    DecodePace pace;
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_EQ(pace.next(2.0, false), Skip::to_key_frame) << i;
        EXPECT_EQ(pace.next(0.0, true), Skip::non_reference) << i;
    }
}

// Frame threads decode the frames nothing depends on alongside the next one: leaving
// those out (measured on a 4K 59.94 fps sample: 32 fps on any number of threads, against
// 47 / 75 fps on one / two with everything) takes the threads' work away.
TEST(DecodePace, WithSeveralThreadsFramesAreNotThinnedButTheDecoderStillJumps)
{
    DecodePace pace;
    pace.allow_thinning(false);
    EXPECT_EQ(pace.next(0.5, false), Skip::none);
    EXPECT_EQ(pace.next(0.5, true), Skip::none);
    EXPECT_EQ(pace.next(2.0, false), Skip::to_key_frame);
    EXPECT_EQ(pace.next(0.1, true), Skip::none);
    pace.reset(); // a seek does not change what the threads can do
    EXPECT_EQ(pace.next(0.5, false), Skip::none);
    pace.allow_thinning(true);
    EXPECT_EQ(pace.next(0.5, false), Skip::non_reference);
}

TEST(DecodePace, ALostDecoderJumpsAtEightTenthsOfASecond)
{
    DecodePace pace;
    EXPECT_EQ(pace.next(0.7, false), Skip::non_reference);
    EXPECT_EQ(pace.next(0.9, false), Skip::to_key_frame);
}

TEST(DecodePace, ASeekStartsOver)
{
    DecodePace pace;
    pace.next(2.0, false);
    ASSERT_EQ(pace.next(0.0, true), Skip::non_reference);
    pace.reset();
    EXPECT_EQ(pace.next(0.0, false), Skip::none);
    pace.next(2.0, false); // in the middle of a jump
    pace.reset();
    EXPECT_EQ(pace.next(0.0, false), Skip::none);
}

} // namespace
