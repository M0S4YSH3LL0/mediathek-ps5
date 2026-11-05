// Mediathek - A film's sound on the console's 8 channels, and back to two.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/surround.hpp"

#include <algorithm>

namespace hui::audio
{

namespace
{

constexpr float kMinus3dB = 0.70710678f;

} // namespace

void place_channels(const float *in, int channels, float *out, std::size_t frames)
{
    std::fill_n(out, frames * kSurroundChannels, 0.0f);
    if (channels != 2 && channels != 6 && channels != 8)
        return;
    const auto count = static_cast<std::size_t>(channels);
    for (std::size_t f = 0; f < frames; ++f)
    {
        const float *from = in + f * count;
        float *to = out + f * kSurroundChannels;
        // Front left, right, centre and LFE have the same places in all of them.
        std::copy_n(from, std::min<std::size_t>(count, 4), to);
        if (channels == 6)
        {
            to[kSurroundLeft] = from[4];
            to[kSurroundRight] = from[5];
        }
        else if (channels == 8)
        {
            to[kBackLeft] = from[4];
            to[kBackRight] = from[5];
            to[kSurroundLeft] = from[6];
            to[kSurroundRight] = from[7];
        }
    }
}

void downmix_frame(const float *frame, float *left, float *right)
{
    const float center = frame[kCenter] * kMinus3dB;
    *left = frame[kFrontLeft] + center + (frame[kSurroundLeft] + frame[kBackLeft]) * kMinus3dB;
    *right = frame[kFrontRight] + center + (frame[kSurroundRight] + frame[kBackRight]) * kMinus3dB;
}

} // namespace hui::audio
