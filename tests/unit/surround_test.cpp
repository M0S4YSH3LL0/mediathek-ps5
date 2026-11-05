// Mediathek - Tests for a film's sound on the console's 8 channels and the mixer's two outputs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/mixer.hpp"
#include "audio/stream_ring.hpp"
#include "audio/surround.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{

using namespace hui::audio;

// FFmpeg's 7.1 puts the backs before the sides; the console wants the sides (its surrounds)
// first. A swap shows as voices from behind, so every channel is pinned.
TEST(Surround, SevenOneGoesIntoTheConsolesOrder)
{
    const float in[8] = {1, 2, 3, 4, 5, 6, 7, 8}; // FL FR FC LFE BL BR SL SR
    float out[8] = {};
    place_channels(in, 8, out, 1);
    const float want[8] = {1, 2, 3, 4, 7, 8, 5, 6}; // L R C LFE Ls Rs Lb Rb
    for (int c = 0; c < 8; ++c)
        EXPECT_EQ(out[c], want[c]) << c;
}

TEST(Surround, FiveOneFillsTheSurroundsAndLeavesTheBacksSilent)
{
    const float in[12] = {1, 2, 3, 4, 5, 6, 11, 12, 13, 14, 15, 16}; // FL FR FC LFE SL SR
    float out[16] = {};
    place_channels(in, 6, out, 2);
    const float want[16] = {1, 2, 3, 4, 5, 6, 0, 0, 11, 12, 13, 14, 15, 16, 0, 0};
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(out[i], want[i]) << i;
}

TEST(Surround, StereoStaysInFrontAndAnUnknownCountIsSilent)
{
    const float in[2] = {0.25f, -0.5f};
    float out[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    place_channels(in, 2, out, 1);
    EXPECT_EQ(out[kFrontLeft], 0.25f);
    EXPECT_EQ(out[kFrontRight], -0.5f);
    for (std::size_t c = kCenter; c < kSurroundChannels; ++c)
        EXPECT_EQ(out[c], 0.0f) << c;
    const float three[3] = {1, 1, 1};
    place_channels(three, 3, out, 1);
    for (float sample : out)
        EXPECT_EQ(sample, 0.0f);
}

TEST(Surround, TheDownmixKeepsTheCentreOnBothSidesAndLeavesTheLfeOut)
{
    float frame[8] = {};
    frame[kCenter] = 1.0f;
    float left = 0.0f;
    float right = 0.0f;
    downmix_frame(frame, &left, &right);
    EXPECT_NEAR(left, 0.7071f, 1e-3f);
    EXPECT_NEAR(right, 0.7071f, 1e-3f);
    frame[kCenter] = 0.0f;
    frame[kLfe] = 1.0f;
    frame[kSurroundRight] = 1.0f;
    downmix_frame(frame, &left, &right);
    EXPECT_EQ(left, 0.0f);
    EXPECT_NEAR(right, 0.7071f, 1e-3f);
}

TEST(StreamRing, CarriesFramesOfEightChannels)
{
    StreamRing ring(4, kSurroundChannels);
    std::vector<float> in(3 * 8);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(i);
    EXPECT_EQ(ring.write(in.data(), 3), 3u);
    EXPECT_EQ(ring.write(in.data(), 3), 1u); // only one frame of room left
    std::vector<float> out(4 * 8);
    EXPECT_EQ(ring.read(out.data(), 4), 4u);
    for (std::size_t i = 0; i < in.size(); ++i)
        EXPECT_EQ(out[i], in[i]) << i;
    for (std::size_t c = 0; c < 8; ++c)
        EXPECT_EQ(out[24 + c], in[c]) << c;
}

// A grain with a constant 0.5 on one channel of the film's sound, and the mixer around it.
class SurroundMixer : public ::testing::Test
{
  protected:
    SurroundMixer() : ring_(4096, kSurroundChannels)
    {
        mixer_.attach_stream(Mixer::kMovieStream, &ring_);
        mixer_.set_stream_gain(Mixer::kMovieStream, 1.0f, 0.0f);
    }
    void fill(std::size_t channel, bool surround)
    {
        std::vector<float> frames(1024 * kSurroundChannels, 0.0f);
        for (std::size_t f = 0; f < 1024; ++f)
            frames[f * kSurroundChannels + channel] = 0.5f;
        ring_.write(frames.data(), 1024);
        ring_.set_surround(surround);
    }
    // One grain for the stereo port, and with `both` one for the 8-channel port.
    void render(bool both)
    {
        stereo_.assign(256 * 2, 0);
        surround_.assign(256 * kSurroundChannels, 0);
        mixer_.render(stereo_.data(), 256, both ? surround_.data() : nullptr);
    }
    // The last frame's sample on a channel.
    static float level(const std::vector<std::int16_t> &out, std::size_t channels,
                       std::size_t channel)
    {
        return static_cast<float>(out[out.size() - channels + channel]) / 32767.0f;
    }
    static float energy(const std::vector<std::int16_t> &out)
    {
        float sum = 0.0f;
        for (std::int16_t sample : out)
            sum += std::abs(static_cast<float>(sample));
        return sum;
    }

    Mixer mixer_;
    StreamRing ring_;
    std::vector<std::int16_t> stereo_;
    std::vector<std::int16_t> surround_;
};

TEST_F(SurroundMixer, ASurroundFilmGoesToTheEightChannelPortOnly)
{
    fill(kSurroundLeft, true);
    EXPECT_TRUE(mixer_.wants_surround());
    render(true);
    EXPECT_NEAR(level(surround_, kSurroundChannels, kSurroundLeft), 0.5f, 0.01f);
    EXPECT_EQ(level(surround_, kSurroundChannels, kFrontLeft), 0.0f);
    EXPECT_EQ(level(surround_, kSurroundChannels, kBackLeft), 0.0f);
    EXPECT_EQ(energy(stereo_), 0.0f);
}

TEST_F(SurroundMixer, AStereoFilmStaysOnTheStereoPort)
{
    fill(kFrontRight, false);
    EXPECT_FALSE(mixer_.wants_surround());
    render(true);
    EXPECT_NEAR(level(stereo_, 2, 1), 0.5f, 0.01f);
    EXPECT_EQ(energy(surround_), 0.0f);
}

// Pausing mutes the film; it keeps its port, which plays silence, so nothing moves between
// ports at play and pause (that crackled on the console).
TEST_F(SurroundMixer, APausedSurroundFilmKeepsItsPortAndSoundsStayOnTheStereoOne)
{
    fill(kCenter, true);
    mixer_.set_stream_gain(Mixer::kMovieStream, 0.0f, 0.0f);
    Tone tone;
    tone.seconds = 1.0f;
    tone.attack = 0.0f;
    mixer_.play_tone(tone, PlayParams{});
    render(true);
    EXPECT_TRUE(mixer_.wants_surround());
    EXPECT_GT(energy(stereo_), 0.0f);
    EXPECT_EQ(energy(surround_), 0.0f);
}

// Without the 8-channel port the film is still heard: mixed down into two channels.
TEST_F(SurroundMixer, WithoutTheEightChannelPortSurroundIsMixedDown)
{
    fill(kCenter, true);
    render(false);
    EXPECT_NEAR(level(stereo_, 2, 0), 0.5f * 0.7071f, 0.01f);
    EXPECT_NEAR(level(stereo_, 2, 1), 0.5f * 0.7071f, 0.01f);
}

} // namespace
