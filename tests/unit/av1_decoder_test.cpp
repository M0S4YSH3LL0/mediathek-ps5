// Mediathek - Tests for the AV1 decoder (dav1d), with real AV1 clips.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The clips in tests/fixtures/av1 are flat test pictures, 96 x 64, made with FFmpeg's
// libaom-av1 at 24 fps from raw YUV:
//   flat-8bit.ivf      12 frames, key frames at 0 and 6 (-g 6), BT.709; luma 100, the left
//                      half 100 + the frame's number; U 90, V 160
//   flat-10bit-pq.ivf  4 frames, 10-bit, BT.2020 PQ; luma 400, U and V 512

#include "platform/ps5/av1_decoder.hpp"

#include "core/save_file.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef HUI_SOURCE_DIR
#define HUI_SOURCE_DIR "."
#endif

namespace
{

using hui::ps5::Av1Decoder;
using hui::ps5::Av1Layout;
using hui::ps5::Av1Picture;
using hui::ps5::Av1Settings;

struct Unit
{
    std::vector<std::uint8_t> data;
    std::int64_t timestamp = 0;
};

// IVF: a 32-byte file header, then each frame as its size (4 bytes), its time (8) and its data.
std::vector<Unit> read_ivf(const char *name)
{
    std::string file;
    const std::string path = std::string(HUI_SOURCE_DIR "/tests/fixtures/av1/") + name;
    EXPECT_TRUE(hui::save::read_file(path, &file)) << path;
    std::vector<Unit> units;
    if (file.size() < 32 || std::memcmp(file.data(), "DKIF", 4) != 0)
        return units;
    const auto byte = [&file](std::size_t i) { return static_cast<std::uint8_t>(file[i]); };
    std::size_t at = static_cast<std::size_t>(byte(6) | (byte(7) << 8));
    while (at + 12 <= file.size())
    {
        std::uint32_t size = 0;
        std::int64_t time = 0;
        std::memcpy(&size, &file[at], 4);
        std::memcpy(&time, &file[at + 4], 8);
        at += 12;
        if (at + size > file.size())
            break;
        Unit unit;
        unit.data.assign(file.begin() + static_cast<std::ptrdiff_t>(at),
                         file.begin() + static_cast<std::ptrdiff_t>(at + size));
        unit.timestamp = time;
        units.push_back(std::move(unit));
        at += size;
    }
    return units;
}

// What the tests look at in a picture, copied out while it is valid.
struct Seen
{
    std::int64_t timestamp = 0;
    int width = 0;
    int height = 0;
    int bits = 0;
    Av1Layout layout = Av1Layout::i420;
    bool wide = false;
    int left_luma = 0; // a sample in the left half, and one in the right
    int right_luma = 0;
    int u = 0;
    int v = 0;
    hui::media::CodedColour colour;
};

int sample(const std::uint8_t *plane, std::size_t pitch, bool wide, int x, int y)
{
    const std::uint8_t *row = plane + static_cast<std::size_t>(y) * pitch;
    if (!wide)
        return row[x];
    std::uint16_t value = 0;
    std::memcpy(&value, row + static_cast<std::size_t>(x) * 2, 2);
    return value;
}

Av1Decoder::Sink collect(std::vector<Seen> *seen)
{
    return [seen](const Av1Picture &picture)
    {
        const hui::media::YuvPicture &p = picture.planes;
        Seen s;
        s.timestamp = picture.timestamp;
        s.width = p.width;
        s.height = p.height;
        s.bits = picture.bits;
        s.layout = picture.layout;
        s.wide = p.wide;
        s.left_luma = sample(p.luma, p.pitch, p.wide, 8, 32);
        s.right_luma = sample(p.luma, p.pitch, p.wide, 80, 32);
        s.u = sample(p.chroma, p.chroma_pitch, p.wide, 20, 16);
        s.v = sample(p.chroma_v, p.chroma_pitch, p.wide, 20, 16);
        s.colour = picture.colour;
        seen->push_back(s);
        return true;
    };
}

std::vector<Seen> decode_all(Av1Decoder &decoder, const std::vector<Unit> &units,
                             std::size_t from = 0)
{
    std::vector<Seen> seen;
    const Av1Decoder::Sink sink = collect(&seen);
    for (std::size_t i = from; i < units.size(); ++i)
        EXPECT_EQ(
            decoder.decode(units[i].data.data(), units[i].data.size(), units[i].timestamp, sink),
            0);
    decoder.drain(sink);
    return seen;
}

std::unique_ptr<Av1Decoder> open(int threads, int delay, hui::media::PicturePool *pool = nullptr)
{
    Av1Settings settings;
    settings.threads = threads;
    settings.frame_delay = delay;
    settings.pool = pool;
    int error = 0;
    auto decoder = Av1Decoder::open(settings, &error);
    EXPECT_NE(decoder, nullptr) << "dav1d error " << error;
    return decoder;
}

// Lossy coding moves a flat value by a step or two.
constexpr int kNear = 3;

TEST(Av1Decoder, EveryFrameComesOutInOrderWithItsTimeAndValues)
{
    const std::vector<Unit> units = read_ivf("flat-8bit.ivf");
    ASSERT_EQ(units.size(), 12u);
    auto decoder = open(1, 1);
    ASSERT_NE(decoder, nullptr);
    const std::vector<Seen> seen = decode_all(*decoder, units);
    ASSERT_EQ(seen.size(), 12u);
    for (std::size_t i = 0; i < seen.size(); ++i)
    {
        SCOPED_TRACE(i);
        EXPECT_EQ(seen[i].timestamp, static_cast<std::int64_t>(i));
        EXPECT_EQ(seen[i].width, 96);
        EXPECT_EQ(seen[i].height, 64);
        EXPECT_EQ(seen[i].bits, 8);
        EXPECT_FALSE(seen[i].wide);
        EXPECT_EQ(seen[i].layout, Av1Layout::i420);
        EXPECT_NEAR(seen[i].left_luma, 100 + static_cast<int>(i), kNear);
        EXPECT_NEAR(seen[i].right_luma, 100, kNear);
        EXPECT_NEAR(seen[i].u, 90, kNear);
        EXPECT_NEAR(seen[i].v, 160, kNear);
        EXPECT_EQ(seen[i].colour.primaries, 1);
        EXPECT_EQ(seen[i].colour.transfer, 1);
        EXPECT_EQ(seen[i].colour.matrix, 1);
        EXPECT_FALSE(seen[i].colour.full_range);
    }
}

TEST(Av1Decoder, FrameThreadsGiveTheSamePicturesLater)
{
    const std::vector<Unit> units = read_ivf("flat-8bit.ivf");
    auto decoder = open(4, 3);
    ASSERT_NE(decoder, nullptr);
    const std::vector<Seen> seen = decode_all(*decoder, units);
    ASSERT_EQ(seen.size(), 12u) << "the last ones come with the drain";
    for (std::size_t i = 0; i < seen.size(); ++i)
    {
        EXPECT_EQ(seen[i].timestamp, static_cast<std::int64_t>(i));
        EXPECT_NEAR(seen[i].left_luma, 100 + static_cast<int>(i), kNear);
    }
}

TEST(Av1Decoder, TenBitPqKeepsItsSamplesAndSaysWhatItIs)
{
    const std::vector<Unit> units = read_ivf("flat-10bit-pq.ivf");
    ASSERT_EQ(units.size(), 4u);
    auto decoder = open(2, 0);
    ASSERT_NE(decoder, nullptr);
    const std::vector<Seen> seen = decode_all(*decoder, units);
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0].bits, 10);
    EXPECT_TRUE(seen[0].wide);
    EXPECT_NEAR(seen[0].left_luma, 400, kNear * 4);
    EXPECT_NEAR(seen[0].u, 512, kNear * 4);
    EXPECT_NEAR(seen[0].v, 512, kNear * 4);
    EXPECT_EQ(seen[0].colour.primaries, 9);
    EXPECT_EQ(seen[0].colour.transfer, 16);
    EXPECT_EQ(seen[0].colour.matrix, 9);
    EXPECT_EQ(hui::media::coded_colour(seen[0].colour).transfer, hui::media::Transfer::pq);
}

TEST(Av1Decoder, AfterAFlushItStartsAgainAtAKeyFrame)
{
    const std::vector<Unit> units = read_ivf("flat-8bit.ivf");
    auto decoder = open(4, 2);
    ASSERT_NE(decoder, nullptr);
    std::vector<Seen> before;
    const Av1Decoder::Sink sink = collect(&before);
    for (std::size_t i = 0; i < 4; ++i)
        decoder->decode(units[i].data.data(), units[i].data.size(), units[i].timestamp, sink);
    decoder->flush(); // a seek: what was in flight is gone
    const std::vector<Seen> after = decode_all(*decoder, units, 6);
    ASSERT_EQ(after.size(), 6u);
    for (std::size_t i = 0; i < after.size(); ++i)
    {
        EXPECT_EQ(after[i].timestamp, static_cast<std::int64_t>(6 + i));
        EXPECT_NEAR(after[i].left_luma, 106 + static_cast<int>(i), kNear);
    }
}

TEST(Av1Decoder, PicturesComeFromThePoolAndAllGoBack)
{
    hui::media::PicturePool::Backend backend;
    int asked = 0;
    backend.allocate = [&asked](std::size_t bytes) -> void *
    {
        ++asked;
        return std::aligned_alloc(64, (bytes + 63) & ~std::size_t{63});
    };
    backend.release = [](void *address, std::size_t) { std::free(address); };
    hui::media::PicturePool pool(std::move(backend), 32);
    {
        auto decoder = open(4, 2, &pool);
        ASSERT_NE(decoder, nullptr);
        const std::vector<Seen> seen = decode_all(*decoder, read_ivf("flat-8bit.ivf"));
        ASSERT_EQ(seen.size(), 12u);
        EXPECT_NEAR(seen[11].left_luma, 111, kNear);
        EXPECT_GT(pool.blocks(), 0u);
        EXPECT_LT(asked, 12) << "blocks are used again";
    }
    EXPECT_EQ(pool.in_use(), 0u) << "closing the decoder gives every picture back";
}

TEST(Av1Decoder, APoolWithNoMemoryFailsTheFilmWithoutCrashing)
{
    hui::media::PicturePool::Backend backend;
    backend.allocate = [](std::size_t) -> void * { return nullptr; };
    backend.release = [](void *, std::size_t) {};
    hui::media::PicturePool pool(std::move(backend), 4);
    auto decoder = open(1, 1, &pool);
    ASSERT_NE(decoder, nullptr);
    const std::vector<Unit> units = read_ivf("flat-8bit.ivf");
    std::vector<Seen> seen;
    EXPECT_LT(decoder->decode(units[0].data.data(), units[0].data.size(), 0, collect(&seen)), 0);
    EXPECT_TRUE(seen.empty());
}

TEST(Av1Decoder, DataThatIsNotAv1IsAnErrorAndTheNextKeyFrameDecodes)
{
    const std::vector<Unit> units = read_ivf("flat-8bit.ivf");
    auto decoder = open(1, 1);
    ASSERT_NE(decoder, nullptr);
    std::vector<Seen> seen;
    const std::vector<std::uint8_t> junk(64, 0xA7);
    EXPECT_LT(decoder->decode(junk.data(), junk.size(), 0, collect(&seen)), 0);
    decoder->flush();
    seen = decode_all(*decoder, units, 6);
    EXPECT_EQ(seen.size(), 6u);
}

TEST(Av1Decoder, ASinkThatWantsNoMoreStopsTheDrain)
{
    const std::vector<Unit> units = read_ivf("flat-8bit.ivf");
    auto decoder = open(4, 4);
    ASSERT_NE(decoder, nullptr);
    int handed = 0;
    const Av1Decoder::Sink sink = [&handed](const Av1Picture &) { return ++handed < 2; };
    for (const Unit &unit : units)
        decoder->decode(unit.data.data(), unit.data.size(), unit.timestamp, sink);
    const int before = handed;
    decoder->drain(
        [&handed](const Av1Picture &)
        {
            ++handed;
            return false;
        });
    EXPECT_LE(handed, before + 1);
}

} // namespace
