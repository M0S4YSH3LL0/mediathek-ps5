// Mediathek - A film's sound on the console's 8 channels, and back to two.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace hui::audio
{

// The console's 8-channel port (sceAudioOut format 2) takes its channels in this order, as
// VLC's PS5 port found: front left and right, centre, LFE, surround (side) left and right,
// back left and right. A 5.1 film leaves the backs silent.
enum SurroundChannel : std::size_t
{
    kFrontLeft = 0,
    kFrontRight,
    kCenter,
    kLfe,
    kSurroundLeft,
    kSurroundRight,
    kBackLeft,
    kBackRight,
    kSurroundChannels,
};

// Frames from FFmpeg's resampler into the console's order: 2 channels (stereo), 6 (5.1:
// FL FR FC LFE SL SR) or 8 (7.1: FL FR FC LFE BL BR SL SR, where FFmpeg puts the backs
// before the sides). Any other count leaves `out` silent. `out` holds frames * 8 samples.
void place_channels(const float *in, int channels, float *out, std::size_t frames);

// One frame in the console's order as two channels, for a stereo output that still gets
// surround frames (the 8-channel port could not be opened, or a grain at the switch):
// centre and surrounds at -3 dB into their side, LFE left out, as FFmpeg's default downmix.
void downmix_frame(const float *frame, float *left, float *right);

} // namespace hui::audio
