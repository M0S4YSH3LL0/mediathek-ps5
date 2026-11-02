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

// How the film codes its light: ordinary video, or one of HDR's two curves.
enum class Transfer : std::uint8_t
{
    sdr,
    pq,  // SMPTE ST 2084: absolute brightness up to 10 000 nits
    hlg, // ARIB STD-B67: relative, made for broadcast
};

// What it takes to make a picture's planes into the colours of an ordinary screen.
struct YuvColour
{
    YuvMatrix matrix = YuvMatrix::bt709;
    bool full_range = false;
    Transfer transfer = Transfer::sdr;
    bool wide_gamut = false;   // BT.2020 primaries, which the screen's BT.709 cannot show as such
    float peak_nits = 1000.0f; // the brightest the film was mastered for; HDR only
};

// The film's own word on its brightest light, from its metadata: the content light level
// if it has one, else the mastering display's peak, else the 1000 nits most films are
// graded for. Kept within what a tone curve can sensibly use.
float hdr_peak_nits(float max_content_light, float mastering_peak);

// The planes as the graphics chip gets them: rows without gaps, the luma plane first
// and then the chroma plane with U and V taking turns, 16-bit samples with their 10 bits
// at the top (so 1.0 is white whatever the decoder wrote).
struct YuvLayout
{
    int luma_width = 0;
    int luma_height = 0;
    int chroma_width = 0; // in pairs
    int chroma_height = 0;
    bool wide = false;
};

// The layout for showing `source` at width x height. A source at least twice the size
// each way has its luma halved here, averaging four samples: 4K arrives as 1080p luma,
// which is half the bytes to copy and to upload, and the chroma plane already has that
// size. Anything smaller is left for the graphics chip to scale.
YuvLayout yuv_layout(const YuvPicture &source, int width, int height);
std::size_t yuv_luma_bytes(const YuvLayout &layout);
std::size_t yuv_bytes(const YuvLayout &layout);
// Writes yuv_bytes(layout) bytes to `out`.
void copy_planes(const YuvPicture &source, const YuvLayout &layout, std::uint8_t *out);

// A picture on its way to the graphics chip.
struct YuvFrame
{
    const std::uint8_t *planes = nullptr; // as copy_planes() wrote them
    YuvLayout layout;
    YuvColour colour;
};

// How far up the 10 bits sit in 16-bit samples. The console's decoder keeps them at the
// bottom of each word, which is not P010; this reads a frame rather than trust that, as
// the wrong answer shows nothing but black or noise.
int ten_bit_shift(const std::uint16_t *samples, std::size_t count);

} // namespace hui::media
