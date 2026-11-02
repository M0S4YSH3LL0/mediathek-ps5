// Mediathek - How much memory the picture decoder needs, and how many threads it may have.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace hui::media
{

// What FFmpeg's decoder holds for pictures of this size: the frames it keeps as
// references and one more for every frame thread, each with the tables that
// come with a frame. `bytes_per_sample` is 1 for 8-bit video and 2 for 10-bit.
// Estimated from measurements of 4K HEVC (see the test), a little on the high
// side on purpose: the heap is fixed and running it out kills the app.
std::size_t decoder_bytes(int width, int height, int bytes_per_sample, int threads);

// The most frame threads, up to `wanted`, whose decoder fits into `available`
// bytes; 0 when not even one does.
int decoder_threads(int width, int height, int bytes_per_sample, int wanted, std::size_t available);

} // namespace hui::media
