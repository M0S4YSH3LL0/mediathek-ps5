// Mediathek - A decoded two-plane picture made into the RGBA picture the screen shows.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace hui::media
{

enum class YuvMatrix
{
    bt601,
    bt709,
    bt2020
};

// A 4:2:0 picture as the hardware decoder writes it: the luma plane, then the chroma
// plane with U and V taking turns, both `pitch` bytes per row.
struct YuvPicture
{
    const std::uint8_t *luma = nullptr;
    const std::uint8_t *chroma = nullptr;
    std::size_t pitch = 0;
    int width = 0; // what the film shows, which can be less than the rows hold
    int height = 0;
    bool wide = false; // 16-bit samples rather than 8
    int shift = 0;     // for 16-bit samples: how far up the 10 bits sit in them
};

// Scales `picture` down to width x height RGBA, four bytes a pixel, no gaps. Each pixel
// averages four luma samples: at exactly half the size, which is 4K on a 1080p screen,
// that is a proper box filter, and the chroma plane already has the size wanted.
void yuv_to_rgba(const YuvPicture &picture, YuvMatrix matrix, bool full_range, std::uint8_t *rgba,
                 int width, int height);

// How far up the 10 bits sit in 16-bit samples. The console's decoder keeps them at the
// bottom of each word, which is not P010; this reads a frame rather than trust that, as
// the wrong answer shows nothing but black or noise.
int ten_bit_shift(const std::uint16_t *samples, std::size_t count);

} // namespace hui::media
