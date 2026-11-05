// Mediathek - Tests for HDR10 output: the PQ curve, the screen's 10-bit words, the switch.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/hdr_output.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace
{

using hui::media::HdrSwitch;

TEST(HdrOutput, PqMatchesTheStandardsKnownPoints)
{
    EXPECT_NEAR(hui::media::pq_from_nits(0.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(hui::media::pq_from_nits(10000.0f), 1.0f, 1e-5f);
    // BT.2408: 203 nits is PQ code 58 %, 100 nits about 51 %.
    EXPECT_NEAR(hui::media::pq_from_nits(203.0f), 0.58f, 0.005f);
    EXPECT_NEAR(hui::media::pq_from_nits(100.0f), 0.508f, 0.005f);
}

TEST(HdrOutput, PqRoundTrips)
{
    for (float nits : {0.5f, 10.0f, 100.0f, 203.0f, 1000.0f, 4000.0f})
        EXPECT_NEAR(hui::media::nits_from_pq(hui::media::pq_from_nits(nits)), nits, nits * 1e-3f)
            << nits;
}

// The scan-out reads A2R10G10B10 with blue in the low bits; a wrong order shows as a blue
// or red cast on the TV, so the layout is pinned here.
TEST(HdrOutput, AWordHoldsBlueLowRedHighAndOpaqueAlpha)
{
    const std::uint32_t word = hui::media::pack_hdr10(1023, 0, 0);
    EXPECT_EQ(word, 0xFFF00000u);
    EXPECT_EQ(hui::media::pack_hdr10(0, 0, 1023), 0xC00003FFu);
    EXPECT_EQ(hui::media::pack_hdr10(0, 1023, 0), 0xC00FFC00u);
    const hui::media::Hdr10Pixel pixel = hui::media::unpack_hdr10(hui::media::pack_hdr10(1, 2, 3));
    EXPECT_EQ(pixel.red, 1);
    EXPECT_EQ(pixel.green, 2);
    EXPECT_EQ(pixel.blue, 3);
}

TEST(HdrOutput, ValuesOutsideTenBitsAreClamped)
{
    const hui::media::Hdr10Pixel pixel =
        hui::media::unpack_hdr10(hui::media::pack_hdr10(2000, -5, 1024));
    EXPECT_EQ(pixel.red, 1023);
    EXPECT_EQ(pixel.green, 0);
    EXPECT_EQ(pixel.blue, 1023);
}

// The screen keeps a pixel's bytes as B, G, R, A; GL reads them back as R, G, B, A.
TEST(HdrOutput, TheWordComesFromTheChannelsAsTheScreenStoresThem)
{
    const std::uint8_t rgba[4] = {0x22, 0x33, 0x44, 0xC0};
    EXPECT_EQ(hui::media::hdr10_word(rgba), 0xC0223344u);
}

TEST(HdrOutput, TheInterfacesWhiteIsWhiteInThePreview)
{
    const int code =
        static_cast<int>(hui::media::pq_from_nits(hui::media::kHdrWhiteNits) * 1023.0f + 0.5f);
    const std::uint32_t word = hui::media::pack_hdr10(code, code, code);
    std::uint8_t px[4] = {static_cast<std::uint8_t>((word >> 16) & 255u),
                          static_cast<std::uint8_t>((word >> 8) & 255u),
                          static_cast<std::uint8_t>(word & 255u),
                          static_cast<std::uint8_t>(word >> 24)};
    hui::media::hdr10_to_viewable(px, 1);
    for (int c = 0; c < 3; ++c)
        EXPECT_GE(px[c], 252) << c;
    EXPECT_EQ(px[3], 255);
}

TEST(HdrSwitch, GoesOnOnlyAfterTheWishHeld)
{
    HdrSwitch hdr;
    EXPECT_FALSE(hdr.update(true, 10.0).has_value());
    EXPECT_FALSE(hdr.update(true, 10.3).has_value());
    const std::optional<bool> change = hdr.update(true, 10.4);
    ASSERT_TRUE(change.has_value());
    EXPECT_TRUE(*change);
    hdr.done(true, true, 10.4);
    EXPECT_TRUE(hdr.on());
    EXPECT_FALSE(hdr.update(true, 11.0).has_value());
}

// A film's picture comes and goes while it starts: a short gap must not blank the TV twice.
TEST(HdrSwitch, AShortGapDoesNotSwitchOff)
{
    HdrSwitch hdr;
    hdr.update(true, 0.0);
    hdr.update(true, 0.5);
    hdr.done(true, true, 0.5);
    EXPECT_FALSE(hdr.update(false, 1.0).has_value());
    EXPECT_FALSE(hdr.update(false, 1.9).has_value());
    EXPECT_FALSE(hdr.update(true, 1.95).has_value());
    EXPECT_TRUE(hdr.on());
    hdr.update(false, 3.0);
    const std::optional<bool> change = hdr.update(false, 4.0);
    ASSERT_TRUE(change.has_value());
    EXPECT_FALSE(*change);
}

TEST(HdrSwitch, ARefusedSwitchIsNotAskedAgainUntilTheWishChanges)
{
    HdrSwitch hdr;
    hdr.update(true, 0.0);
    ASSERT_TRUE(hdr.update(true, 0.4).has_value());
    hdr.done(true, false, 0.4);
    EXPECT_FALSE(hdr.on());
    EXPECT_FALSE(hdr.update(true, 5.0).has_value());
    EXPECT_FALSE(hdr.update(false, 6.0).has_value());
    hdr.update(true, 7.0);
    EXPECT_TRUE(hdr.update(true, 7.4).has_value());
}

TEST(HdrSwitch, AFailedSwitchBackIsTriedAgainAfterAnotherHold)
{
    HdrSwitch hdr;
    hdr.update(true, 0.0);
    hdr.update(true, 0.4);
    hdr.done(true, true, 0.4);
    hdr.update(false, 1.0);
    ASSERT_TRUE(hdr.update(false, 2.0).has_value());
    hdr.done(false, false, 2.0);
    EXPECT_TRUE(hdr.on());
    EXPECT_FALSE(hdr.update(false, 2.5).has_value());
    EXPECT_TRUE(hdr.update(false, 3.0).has_value());
}

} // namespace
