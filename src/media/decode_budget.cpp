// Mediathek - How much memory the picture decoder needs, and how many threads it may have.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/decode_budget.hpp"

#include <algorithm>

namespace hui::media
{

namespace
{

// HEVC codes in blocks of up to 64 x 64 and allocates whole blocks.
std::size_t round_up_64(int value)
{
    return (static_cast<std::size_t>(value) + 63u) & ~static_cast<std::size_t>(63u);
}

} // namespace

std::size_t decoder_bytes(int width, int height, int bytes_per_sample, int threads)
{
    if (width <= 0 || height <= 0)
        return 0;
    const std::size_t pixels = round_up_64(width) * round_up_64(height);
    // 4:2:0 samples, plus about half a byte a pixel of motion and block tables.
    const std::size_t frame =
        pixels * 3 / 2 * static_cast<std::size_t>(std::max(bytes_per_sample, 1)) + pixels / 2;
    // Fitted to measurements: three frames that do not depend on the threads
    // (references, the one the player holds) and two and a half more for each
    // thread, counted in halves. The console's own peak, not the PC's, sets the
    // first number: one 10-bit 4K thread took at most 5 frames' worth there.
    return frame * (6u + 5u * static_cast<std::size_t>(std::max(threads, 1))) / 2;
}

int decoder_threads(int width, int height, int bytes_per_sample, int wanted, std::size_t available)
{
    for (int threads = std::max(wanted, 1); threads >= 1; --threads)
    {
        if (decoder_bytes(width, height, bytes_per_sample, threads) <= available)
            return threads;
    }
    return 0;
}

} // namespace hui::media
