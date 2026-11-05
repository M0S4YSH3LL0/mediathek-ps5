// Mediathek - Tests for making a decoded two-plane picture into RGBA.
// Copyright (C) 2026 BlackBearReloaded SPDX-License-Identifier: GPL-3.0-or-later

#include "media/yuv_picture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

TEST(YuvPicture, AtItsOwnSizeEachPixelIsItsOwnSample)
{
    Flat<std::uint8_t> flat(4, 4, 16, 128, 128);
    flat.luma[0] = 235;
    flat.luma[13] = 235; // the row below, one to the right: stride is 4 + 8
    const auto rgba = convert(flat.picture, 4, 4);
    EXPECT_EQ(rgba[0], 255) << "white, not mixed with its black neighbours";
    EXPECT_EQ(rgba[4], 0) << "the next pixel is black";
    EXPECT_EQ(rgba[16], 0) << "the one below is black";
    EXPECT_EQ(rgba[20], 255) << "and the one below the next is white";
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

TEST(YuvLayout, FourKOnAFullHdScreenArrivesWithItsLumaHalved)
{
    YuvPicture source;
    source.width = 3840;
    source.height = 2160;
    source.wide = true;
    const YuvLayout layout = yuv_layout(source, 1920, 1080);
    EXPECT_EQ(layout.luma_width, 1920);
    EXPECT_EQ(layout.luma_height, 1080);
    EXPECT_EQ(layout.chroma_width, 1920);
    EXPECT_EQ(layout.chroma_height, 1080);
    EXPECT_TRUE(layout.wide);
    EXPECT_EQ(yuv_luma_bytes(layout), 1920u * 1080u * 2u);
    EXPECT_EQ(yuv_bytes(layout), 1920u * 1080u * 2u * 3u);
}

TEST(YuvLayout, LessThanTwiceTheSizeIsLeftForTheGraphicsChipToScale)
{
    YuvPicture source;
    source.width = 2560;
    source.height = 1440;
    const YuvLayout layout = yuv_layout(source, 1920, 1080);
    EXPECT_EQ(layout.luma_width, 2560);
    EXPECT_EQ(layout.luma_height, 1440);
    EXPECT_EQ(layout.chroma_width, 1280);
    EXPECT_EQ(layout.chroma_height, 720);
    EXPECT_EQ(yuv_bytes(layout), 2560u * 1440u * 3u / 2u);

    source.width = 0;
    EXPECT_EQ(yuv_bytes(yuv_layout(source, 1920, 1080)), 0u);
}

TEST(YuvLayout, AnOddSizeKeepsItsLastChromaColumn)
{
    YuvPicture source;
    source.width = 7;
    source.height = 5;
    const YuvLayout layout = yuv_layout(source, 7, 5);
    EXPECT_EQ(layout.chroma_width, 4);
    EXPECT_EQ(layout.chroma_height, 3);
}

TEST(CopyPlanes, HalvedLumaIsTheAverageOfFourAndTheChromaIsCopiedWithoutTheGaps)
{
    Flat<std::uint8_t> flat(4, 4, 100, 90, 160);
    // One square of four different samples at the top left.
    flat.luma[0] = 10;
    flat.luma[1] = 20;
    const std::size_t stride = flat.picture.pitch;
    flat.luma[stride] = 30;
    flat.luma[stride + 1] = 41;
    const YuvLayout layout = yuv_layout(flat.picture, 2, 2);
    ASSERT_EQ(layout.luma_width, 2);
    std::vector<std::uint8_t> out(yuv_bytes(layout), 0xee);
    copy_planes(flat.picture, layout, out.data());
    EXPECT_EQ(out[0], 25) << "(10 + 20 + 30 + 41 + 2) / 4";
    EXPECT_EQ(out[1], 100);
    EXPECT_EQ(out[2], 100);
    EXPECT_EQ(out[3], 100);
    const std::vector<std::uint8_t> chroma(out.begin() + 4, out.end());
    EXPECT_EQ(chroma, (std::vector<std::uint8_t>{90, 160, 90, 160, 90, 160, 90, 160}));
}

TEST(CopyPlanes, TenBitSamplesEndUpAtTheTopOfTheWordWhereverTheyWere)
{
    for (int shift : {0, 6})
    {
        Flat<std::uint16_t> wide(4, 2, static_cast<std::uint16_t>(940 << shift),
                                 static_cast<std::uint16_t>(64 << shift),
                                 static_cast<std::uint16_t>(960 << shift));
        wide.picture.shift = shift;
        const YuvLayout layout = yuv_layout(wide.picture, 4, 2);
        std::vector<std::uint8_t> out(yuv_bytes(layout), 0);
        copy_planes(wide.picture, layout, out.data());
        std::vector<std::uint16_t> samples(out.size() / 2);
        std::memcpy(samples.data(), out.data(), out.size());
        ASSERT_EQ(samples.size(), 8u + 4u);
        for (int i = 0; i < 8; ++i)
            EXPECT_EQ(samples[static_cast<std::size_t>(i)], 940 << 6) << "shift " << shift;
        EXPECT_EQ(samples[8], 64 << 6);
        EXPECT_EQ(samples[9], 960 << 6);
    }
}

TEST(CopyPlanes, APlanarPictureHasItsUAndVInterleavedAndRaised)
{
    // FFmpeg's 10-bit 4:2:0: three planes, 10 bits at the bottom of each word, rows
    // longer than the picture.
    constexpr int kWidth = 4;
    constexpr int kHeight = 4;
    constexpr int kLumaStride = 6;
    constexpr int kChromaStride = 4;
    std::vector<std::uint16_t> luma(kLumaStride * kHeight, 0);
    std::vector<std::uint16_t> u(kChromaStride * kHeight / 2, 0);
    std::vector<std::uint16_t> v(kChromaStride * kHeight / 2, 0);
    for (int row = 0; row < kHeight; ++row)
        for (int x = 0; x < kWidth; ++x)
            luma[static_cast<std::size_t>(row * kLumaStride + x)] = 500;
    for (int row = 0; row < kHeight / 2; ++row)
        for (int x = 0; x < kWidth / 2; ++x)
        {
            u[static_cast<std::size_t>(row * kChromaStride + x)] =
                static_cast<std::uint16_t>(100 + x);
            v[static_cast<std::size_t>(row * kChromaStride + x)] =
                static_cast<std::uint16_t>(900 + row);
        }
    YuvPicture source;
    source.luma = reinterpret_cast<const std::uint8_t *>(luma.data());
    source.pitch = kLumaStride * 2;
    source.chroma = reinterpret_cast<const std::uint8_t *>(u.data());
    source.chroma_v = reinterpret_cast<const std::uint8_t *>(v.data());
    source.chroma_pitch = kChromaStride * 2;
    source.width = kWidth;
    source.height = kHeight;
    source.wide = true;
    const YuvLayout layout = yuv_layout(source, kWidth, kHeight);
    std::vector<std::uint8_t> out(yuv_bytes(layout), 0);
    copy_planes(source, layout, out.data());
    std::vector<std::uint16_t> samples(out.size() / 2);
    std::memcpy(samples.data(), out.data(), out.size());
    ASSERT_EQ(samples.size(), 16u + 8u);
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(samples[static_cast<std::size_t>(i)], 500 << 6);
    const std::vector<std::uint16_t> chroma(samples.begin() + 16, samples.end());
    EXPECT_EQ(chroma, (std::vector<std::uint16_t>{100 << 6, 900 << 6, 101 << 6, 900 << 6, 100 << 6,
                                                  901 << 6, 101 << 6, 901 << 6}));
}

TEST(CopyPlanes, APlanar4kPictureHasItsLumaHalvedForA1080pScreen)
{
    Flat<std::uint8_t> flat(8, 4, 60, 0, 0);
    std::vector<std::uint8_t> u(8, 70);
    std::vector<std::uint8_t> v(8, 80);
    flat.picture.chroma = u.data();
    flat.picture.chroma_v = v.data();
    flat.picture.chroma_pitch = 4;
    const YuvLayout layout = yuv_layout(flat.picture, 4, 2);
    ASSERT_EQ(layout.luma_width, 4);
    ASSERT_EQ(layout.chroma_width, 4);
    std::vector<std::uint8_t> out(yuv_bytes(layout), 0);
    copy_planes(flat.picture, layout, out.data());
    for (std::size_t i = 0; i < 8; ++i)
        EXPECT_EQ(out[i], 60);
    for (std::size_t i = 8; i < out.size(); i += 2)
    {
        EXPECT_EQ(out[i], 70);
        EXPECT_EQ(out[i + 1], 80);
    }
}

TEST(HdrPeak, TheContentLightLevelThenTheMasteringPeakThenAThousandNits)
{
    EXPECT_FLOAT_EQ(hdr_peak_nits(1500.0f, 4000.0f), 1500.0f);
    EXPECT_FLOAT_EQ(hdr_peak_nits(0.0f, 4000.0f), 4000.0f);
    EXPECT_FLOAT_EQ(hdr_peak_nits(0.0f, 0.0f), 1000.0f);
    EXPECT_FLOAT_EQ(hdr_peak_nits(80.0f, 0.0f), 400.0f) << "too dark for an HDR curve";
    EXPECT_FLOAT_EQ(hdr_peak_nits(0.0f, 50000.0f), 10000.0f);
}

} // namespace
