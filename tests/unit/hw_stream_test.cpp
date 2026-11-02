// Mediathek - Tests for what the hardware picture decoder takes and how it is fed.
// Copyright (C) 2026 BlackBearReloaded SPDX-License-Identifier: GPL-3.0-or-later

#include "media/hw_stream.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace
{

using namespace hui::media;
using Bytes = std::vector<std::uint8_t>;

// Start code, NAL header for `type`, then the rest.
void add_nal(Bytes *unit, int type, std::initializer_list<std::uint8_t> rest = {0x80})
{
    unit->insert(unit->end(), {0, 0, 0, 1, static_cast<std::uint8_t>(type << 1), 1});
    unit->insert(unit->end(), rest);
}

Bytes cleaned(const Bytes &unit)
{
    Bytes out(unit.size() + 4, 0xee);
    out.resize(clean_hevc_unit(unit.data(), unit.size(), out.data()));
    return out;
}

TEST(HwMode, OnlyFilmsBeyond1080pAskForTheHardware)
{
    EXPECT_FALSE(wants_hw_decoder(1920, 1080));
    EXPECT_FALSE(wants_hw_decoder(1920, 1088));
    EXPECT_FALSE(wants_hw_decoder(1280, 720));
    EXPECT_TRUE(wants_hw_decoder(2560, 1440));
    EXPECT_TRUE(wants_hw_decoder(3840, 2160));
}

TEST(HwMode, FourKHevcIsSizedForTheCodedPictureAtLevelFiveTwoThenFiveOne)
{
    const auto main10 = hw_mode(HwCodec::hevc, 2, 10, true, 3840, 2160);
    ASSERT_TRUE(main10.has_value());
    EXPECT_EQ(main10->profile, 2u);
    EXPECT_EQ(main10->level, 156u);
    EXPECT_EQ(main10->fallback_level, 153u);
    EXPECT_EQ(main10->max_width, 3840);
    EXPECT_EQ(main10->max_height, 2176);

    const auto main = hw_mode(HwCodec::hevc, 1, 8, true, 3840, 2160);
    ASSERT_TRUE(main.has_value());
    EXPECT_EQ(main->profile, 1u);
}

// An encoder that pads to 64 codes a 1610-high film as 1632 rows, and a decoder sized
// for 1616 refuses every picture.
TEST(HwMode, ScopeFilmsArePaddedToTheLargestBlock)
{
    const auto scope = hw_mode(HwCodec::hevc, 1, 8, true, 3840, 1610);
    ASSERT_TRUE(scope.has_value());
    EXPECT_EQ(scope->max_height, 1664);
    EXPECT_EQ(scope->max_width, 3840);
}

TEST(HwMode, H264UpToHighIsTakenAndNothingAboveIt)
{
    for (int profile : {66, 77, 100, 578, -99})
        EXPECT_TRUE(hw_mode(HwCodec::h264, profile, 8, true, 3840, 2160).has_value()) << profile;
    EXPECT_EQ(hw_mode(HwCodec::h264, 100, 8, true, 3840, 2160)->level, 52u);
    EXPECT_FALSE(hw_mode(HwCodec::h264, 110, 10, true, 3840, 2160).has_value()) << "High 10";
    EXPECT_FALSE(hw_mode(HwCodec::h264, 122, 8, true, 3840, 2160).has_value()) << "High 4:2:2";
    EXPECT_FALSE(hw_mode(HwCodec::h264, 100, 8, false, 3840, 2160).has_value());
}

TEST(HwMode, WhatTheHardwareCannotPlayStaysWithFfmpeg)
{
    EXPECT_FALSE(hw_mode(HwCodec::hevc, 4, 10, true, 3840, 2160).has_value()) << "range extensions";
    EXPECT_FALSE(hw_mode(HwCodec::hevc, -99, 12, true, 3840, 2160).has_value()) << "12 bit";
    EXPECT_FALSE(hw_mode(HwCodec::hevc, 2, 10, false, 3840, 2160).has_value()) << "4:2:2";
    EXPECT_FALSE(hw_mode(HwCodec::hevc, 2, 10, true, 4096, 2160).has_value()) << "too wide";
    EXPECT_FALSE(hw_mode(HwCodec::hevc, 2, 10, true, 7680, 4320).has_value()) << "8K";
    EXPECT_FALSE(hw_mode(HwCodec::hevc, 2, 10, true, 0, 0).has_value());
}

TEST(HwMode, AnUnknownHevcProfileFollowsTheDepth)
{
    EXPECT_EQ(hw_mode(HwCodec::hevc, -99, 10, true, 3840, 2160)->profile, 2u);
    EXPECT_EQ(hw_mode(HwCodec::hevc, -99, 8, true, 3840, 2160)->profile, 1u);
}

TEST(HevcUnit, ThePictureTypeIsThatOfTheFirstPictureNotOfWhatComesBeforeIt)
{
    Bytes unit;
    add_nal(&unit, 32);
    add_nal(&unit, 33);
    add_nal(&unit, 34);
    add_nal(&unit, 39); // SEI
    add_nal(&unit, 21); // CRA
    add_nal(&unit, 1);
    EXPECT_EQ(hevc_picture_type(unit.data(), unit.size()), 21);

    Bytes none;
    add_nal(&none, 32);
    EXPECT_EQ(hevc_picture_type(none.data(), none.size()), -1);
    EXPECT_EQ(hevc_picture_type(none.data(), 0), -1);
}

TEST(HevcUnit, AnOrdinaryUnitIsCopiedUnchanged)
{
    Bytes unit;
    add_nal(&unit, 34, {0x80, 0x11});
    add_nal(&unit, 1, {0x22, 0x33, 0x44});
    EXPECT_EQ(cleaned(unit), unit);
}

TEST(HevcUnit, ThreeByteStartCodesAndTrailingBytesSurvive)
{
    const Bytes unit = {0, 0, 1, 2, 1, 0xaa, 0, 0, 1, 2, 1, 0xbb, 0xcc};
    EXPECT_EQ(cleaned(unit), unit);
}

TEST(HevcUnit, DolbyVisionsNalUnitsAreLeftOut)
{
    Bytes unit;
    add_nal(&unit, 1, {0x22});
    Bytes expected = unit;
    add_nal(&unit, 62, {0x55, 0x66});
    add_nal(&unit, 63, {0x77});
    EXPECT_EQ(cleaned(unit), expected);
}

// The second header byte holds the layer: Dolby Vision profile 7's enhancement layer is
// layer 1, and the decoder takes the base layer only.
TEST(HevcUnit, NalUnitsOfOtherLayersAreLeftOut)
{
    Bytes unit;
    add_nal(&unit, 1, {0x22});
    Bytes expected = unit;
    unit.insert(unit.end(), {0, 0, 0, 1, 1 << 1, (1 << 3) | 1, 0x33});  // a slice of layer 1
    unit.insert(unit.end(), {0, 0, 0, 1, 33 << 1, (1 << 3) | 1, 0x44}); // its SPS
    EXPECT_EQ(cleaned(unit), expected);
}

// The first slice decides: slice_type is the second exp-Golomb number of its header.
TEST(H264Unit, AnIdrOrAPictureOfISlicesIsAKeyAndNothingElse)
{
    const auto unit = [](std::initializer_list<std::uint8_t> nals)
    {
        Bytes out = {0, 0, 0, 1, 0x67, 0x64}; // an SPS first, as after a seek
        out.insert(out.end(), nals);
        return out;
    };
    // first_mb_in_slice 0 is "1"; slice_type 7 (I) is "0001000", 5 (P) "00110", 6 (B) "00111".
    const Bytes idr = unit({0, 0, 1, 0x65, 0x88, 0x80});
    const Bytes i_slices = unit({0, 0, 1, 0x41, 0x88, 0x80});
    const Bytes p_slices = unit({0, 0, 1, 0x41, 0x9a, 0x00});
    const Bytes b_slices = unit({0, 0, 1, 0x41, 0x9e, 0x00});
    const Bytes no_picture = unit({});
    EXPECT_TRUE(h264_is_key(idr.data(), idr.size()));
    EXPECT_TRUE(h264_is_key(i_slices.data(), i_slices.size()));
    EXPECT_FALSE(h264_is_key(p_slices.data(), p_slices.size()));
    EXPECT_FALSE(h264_is_key(b_slices.data(), b_slices.size()));
    EXPECT_FALSE(h264_is_key(no_picture.data(), no_picture.size()));
}

// PPS id 0 is the single bit 1, id 1 is 010. A remux holds the header's copy and the
// stream's own: the last is what the stream means.
TEST(HevcUnit, AParameterSetALaterOneReplacesIsLeftOut)
{
    Bytes unit;
    add_nal(&unit, 34, {0x80, 0x01});
    Bytes expected;
    add_nal(&unit, 34, {0x40, 0x02});
    add_nal(&expected, 34, {0x40, 0x02});
    add_nal(&unit, 34, {0x80, 0x03});
    add_nal(&expected, 34, {0x80, 0x03});
    add_nal(&unit, 1, {0x22});
    add_nal(&expected, 1, {0x22});
    EXPECT_EQ(cleaned(unit), expected);
}

TEST(HevcUnit, AUnitWithoutStartCodesIsPassedOnAsItIs)
{
    const Bytes unit = {1, 2, 3, 4, 5};
    EXPECT_EQ(cleaned(unit), unit);
}

TEST(HevcEntry, LeadingPicturesAfterARandomAccessStayOutUntilTheFirstTrailingOne)
{
    HevcEntry entry;
    entry.restart();
    EXPECT_TRUE(entry.admit(21)) << "the key frame";
    EXPECT_FALSE(entry.admit(8)) << "RASL_N";
    EXPECT_FALSE(entry.admit(9)) << "RASL_R";
    EXPECT_TRUE(entry.admit(6)) << "RADL can be decoded";
    EXPECT_TRUE(entry.admit(1)) << "trailing";
    // Past that point a RASL refers to what was decoded.
    EXPECT_TRUE(entry.admit(21));
    EXPECT_TRUE(entry.admit(9));
    entry.restart();
    EXPECT_FALSE(entry.admit(9));
}

TEST(HevcEntry, ARefusedKeyFrameHoldsEverythingUntilTheNextOne)
{
    HevcEntry entry;
    entry.restart();
    EXPECT_TRUE(entry.admit(21));
    entry.refused();
    EXPECT_FALSE(entry.admit(1));
    EXPECT_FALSE(entry.admit(9));
    EXPECT_TRUE(entry.admit(19)) << "IDR";
    EXPECT_TRUE(entry.admit(1));
}

TEST(HevcEntry, AUnitWithNoPictureIsNotHeldBack)
{
    HevcEntry entry;
    entry.refused();
    EXPECT_TRUE(entry.admit(-1));
}

TEST(PendingStamps, PicturesInDisplayOrderGetTheEarliestTimeStillOut)
{
    PendingStamps stamps;
    double seconds = 0.0;
    EXPECT_FALSE(stamps.take(&seconds));
    for (double stamp : {0.0, 0.16, 0.08, 0.04, 0.12})
        stamps.push(stamp); // decode order of I P B B B
    for (double expected : {0.0, 0.04, 0.08, 0.12, 0.16})
    {
        ASSERT_TRUE(stamps.take(&seconds));
        EXPECT_DOUBLE_EQ(seconds, expected);
    }
    EXPECT_FALSE(stamps.take(&seconds));
}

TEST(PendingStamps, ALostPictureDoesNotHoldEveryLaterOneBackForever)
{
    PendingStamps stamps;
    stamps.push(-1.0); // the decoder never gave this one back
    for (int i = 0; i < 200; ++i)
        stamps.push(i);
    double seconds = -2.0;
    ASSERT_TRUE(stamps.take(&seconds));
    EXPECT_GE(seconds, 0.0);
    stamps.reset();
    EXPECT_FALSE(stamps.take(&seconds));
}

} // namespace
