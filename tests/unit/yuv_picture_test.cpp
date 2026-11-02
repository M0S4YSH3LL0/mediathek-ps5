// Mediathek - Tests for making a decoded two-plane picture into RGBA.
// Copyright (C) 2026 BlackBearReloaded SPDX-License-Identifier: GPL-3.0-or-later

#include "media/yuv_picture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace
{

using namespace hui::media;

// A picture of one colour in the layout the decoder writes, with rows longer than the
// film (the decoder's pitch) and more of them (its coded height).
template <class Sample> struct Flat
{
    static constexpr int kPad = 8;
    static constexpr int kExtraRows = 4;
    std::vector<Sample> luma, chroma;
    YuvPicture picture;

    Flat(int width, int height, Sample y, Sample u, Sample v)
    {
        const int stride = width + kPad;
        luma.assign(static_cast<std::size_t>(stride * (height + kExtraRows)), Sample{0});
        chroma.assign(static_cast<std::size_t>(stride * (height + kExtraRows) / 2), Sample{0});
        for (int row = 0; row < height; ++row)
            std::fill_n(luma.begin() + row * stride, width, y);
        for (int row = 0; row < height / 2; ++row)
            for (int x = 0; x < width; x += 2)
            {
                chroma[static_cast<std::size_t>(row * stride + x)] = u;
                chroma[static_cast<std::size_t>(row * stride + x + 1)] = v;
            }
        picture.luma = reinterpret_cast<const std::uint8_t *>(luma.data());
        picture.chroma = reinterpret_cast<const std::uint8_t *>(chroma.data());
        picture.pitch = static_cast<std::size_t>(stride) * sizeof(Sample);
        picture.width = width;
        picture.height = height;
        picture.wide = sizeof(Sample) == 2;
    }
};

std::vector<std::uint8_t> convert(const YuvPicture &picture, int width, int height,
                                  YuvMatrix matrix = YuvMatrix::bt709, bool full = false)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width * height) * 4, 0);
    yuv_to_rgba(picture, matrix, full, rgba.data(), width, height);
    return rgba;
}

void expect_every_pixel(const std::vector<std::uint8_t> &rgba, int r, int g, int b,
                        int tolerance = 1)
{
    for (std::size_t i = 0; i < rgba.size(); i += 4)
    {
        ASSERT_LE(std::abs(rgba[i] - r), tolerance) << "red at " << i / 4;
        ASSERT_LE(std::abs(rgba[i + 1] - g), tolerance) << "green at " << i / 4;
        ASSERT_LE(std::abs(rgba[i + 2] - b), tolerance) << "blue at " << i / 4;
        ASSERT_EQ(rgba[i + 3], 255) << "alpha at " << i / 4;
    }
}

TEST(YuvPicture, LimitedRangeBlackAndWhiteAreTheEndsOfTheScale)
{
    Flat<std::uint8_t> black(16, 8, 16, 128, 128);
    expect_every_pixel(convert(black.picture, 8, 4), 0, 0, 0);
    Flat<std::uint8_t> white(16, 8, 235, 128, 128);
    expect_every_pixel(convert(white.picture, 8, 4), 255, 255, 255);
}

// BT.709 red, limited range, from the standard's own table.
TEST(YuvPicture, RedComesOutRedInTheMatrixTheFilmAsksFor)
{
    Flat<std::uint8_t> red(16, 8, 63, 102, 240);
    expect_every_pixel(convert(red.picture, 8, 4), 255, 0, 0, 3);
    // The same numbers through another matrix are another colour.
    const auto other = convert(red.picture, 8, 4, YuvMatrix::bt601);
    EXPECT_LT(other[0], 240);
}

TEST(YuvPicture, FullRangeUsesTheWholeScale)
{
    Flat<std::uint8_t> black(16, 8, 0, 128, 128);
    expect_every_pixel(convert(black.picture, 8, 4, YuvMatrix::bt709, true), 0, 0, 0);
    Flat<std::uint8_t> white(16, 8, 255, 128, 128);
    expect_every_pixel(convert(white.picture, 8, 4, YuvMatrix::bt709, true), 255, 255, 255);
}

TEST(YuvPicture, TenBitSamplesGiveTheSameColoursAsEightBit)
{
    Flat<std::uint16_t> white(16, 8, 940, 512, 512);
    expect_every_pixel(convert(white.picture, 8, 4), 255, 255, 255);
    Flat<std::uint16_t> black(16, 8, 64, 512, 512);
    expect_every_pixel(convert(black.picture, 8, 4), 0, 0, 0);
    Flat<std::uint16_t> red(16, 8, 250, 409, 960);
    expect_every_pixel(convert(red.picture, 8, 4, YuvMatrix::bt709), 255, 0, 0, 3);
}

TEST(YuvPicture, SamplesKeptAtTheTopOfTheWordGiveTheSameColours)
{
    Flat<std::uint16_t> white(16, 8, 940 << 6, 512 << 6, 512 << 6);
    white.picture.shift = 6;
    expect_every_pixel(convert(white.picture, 8, 4), 255, 255, 255);
}

TEST(YuvPicture, AtHalfTheSizeEachPixelIsTheMeanOfItsFourLumaSamples)
{
    Flat<std::uint8_t> flat(4, 4, 16, 128, 128);
    flat.luma[0] = 235;
    flat.luma[1] = 16;
    flat.luma[12] = 235; // the row below: stride is 4 + 8
    flat.luma[13] = 16;
    const auto rgba = convert(flat.picture, 2, 2, YuvMatrix::bt709, false);
    // (235 + 16 + 235 + 16) / 4 = 125.5, half way up the scale: a mid grey.
    EXPECT_NEAR(rgba[0], 127, 2);
    EXPECT_EQ(rgba[4], 0) << "the next pixel is black";
}

TEST(YuvPicture, ChromaFromTheRightPlaceAtHalfTheSize)
{
    Flat<std::uint8_t> flat(8, 4, 128, 128, 128);
    // The second output pixel's chroma, in the first chroma row.
    flat.chroma[2] = 90;
    flat.chroma[3] = 240;
    const auto rgba = convert(flat.picture, 4, 2);
    EXPECT_GT(rgba[4], rgba[0] + 50) << "more red than its neighbour";
    EXPECT_EQ(rgba[8], rgba[0]) << "the next one is grey again";
}

TEST(YuvPicture, ACropThatIsNotHalfNeverReadsBeyondThePicture)
{
    // 2560 x 1440 on a 1080p screen, in small: 4:3 ratios, the general path.
    Flat<std::uint8_t> flat(64, 36, 235, 128, 128);
    expect_every_pixel(convert(flat.picture, 48, 27), 255, 255, 255);
    Flat<std::uint16_t> wide(64, 36, 64, 512, 512);
    expect_every_pixel(convert(wide.picture, 48, 27), 0, 0, 0);
}

TEST(YuvPicture, TheEdgesOfTheCodedRowsAreNeverShown)
{
    // The rows under the film hold zero (below the black level): they would show as a line.
    Flat<std::uint8_t> flat(32, 16, 235, 128, 128);
    expect_every_pixel(convert(flat.picture, 16, 8), 255, 255, 255);
}

TEST(YuvPicture, SizesThatMakeNoSenseWriteNothing)
{
    Flat<std::uint8_t> flat(4, 4, 16, 128, 128);
    std::uint8_t rgba[4] = {1, 2, 3, 4};
    yuv_to_rgba(flat.picture, YuvMatrix::bt709, false, rgba, 0, 0);
    EXPECT_EQ(rgba[0], 1);
}

TEST(TenBitShift, ReadsWhereTheBitsAreSitting)
{
    const std::uint16_t low[] = {64, 512, 940, 1023};
    EXPECT_EQ(ten_bit_shift(low, 4), 0);
    const std::uint16_t high[] = {64 << 6, 512 << 6, 940 << 6, 1023 << 6};
    EXPECT_EQ(ten_bit_shift(high, 4), 6);
    const std::uint16_t empty[] = {0, 0};
    EXPECT_EQ(ten_bit_shift(empty, 2), 0) << "an all-black row says nothing: the usual layout";
}

} // namespace
