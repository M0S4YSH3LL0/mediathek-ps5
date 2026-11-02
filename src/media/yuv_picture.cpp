// Mediathek - A decoded two-plane picture made into the RGBA picture the screen shows.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/yuv_picture.hpp"

#include <algorithm>

namespace hui::media
{

namespace
{

// Eighth-bit fixed point: the matrices' factors times 256.
struct Factors
{
    int luma, red_v, green_u, green_v, blue_u, black;
};

Factors factors(YuvMatrix matrix, bool full)
{
    switch (matrix)
    {
    case YuvMatrix::bt601:
        return full ? Factors{256, 359, 88, 183, 454, 0} : Factors{298, 409, 100, 208, 516, 16};
    case YuvMatrix::bt2020:
        return full ? Factors{256, 377, 42, 146, 482, 0} : Factors{298, 430, 48, 167, 548, 16};
    case YuvMatrix::bt709:
        break;
    }
    return full ? Factors{256, 403, 48, 120, 475, 0} : Factors{298, 459, 55, 136, 541, 16};
}

inline std::uint8_t clamped(int value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

// Samples of 8 bits, or of 10 kept in 16: both come out as 8.
template <class Sample> struct Reading
{
    static constexpr int kExtra = sizeof(Sample) == 1 ? 0 : 2;
};

// The common case gets a loop of its own: nothing in it depends on the pixel before,
// so the compiler can do eight pixels at once. A 4K picture has no time to spare.
template <class Sample>
void half_row(const Sample *top, const Sample *bottom, const Sample *chroma, int shift,
              const Factors &f, std::uint8_t *out, int width)
{
    constexpr int extra = Reading<Sample>::kExtra;
    constexpr int round = 2 << extra;
    for (int x = 0; x < width; ++x)
    {
        const int sum = (top[2 * x] >> shift) + (top[2 * x + 1] >> shift) +
                        (bottom[2 * x] >> shift) + (bottom[2 * x + 1] >> shift);
        const int y = f.luma * (((sum + round) >> (2 + extra)) - f.black) + 128;
        const int u = (chroma[2 * x] >> (shift + extra)) - 128;
        const int v = (chroma[2 * x + 1] >> (shift + extra)) - 128;
        out[4 * x] = clamped((y + f.red_v * v) >> 8);
        out[4 * x + 1] = clamped((y - f.green_u * u - f.green_v * v) >> 8);
        out[4 * x + 2] = clamped((y + f.blue_u * u) >> 8);
        out[4 * x + 3] = 255;
    }
}

template <class Sample>
void any_row(const Sample *top, const Sample *bottom, const Sample *chroma, int shift,
             const Factors &f, std::uint8_t *out, int width, int source_width)
{
    constexpr int extra = Reading<Sample>::kExtra;
    constexpr int round = 2 << extra;
    const std::int64_t step = (static_cast<std::int64_t>(source_width) << 16) / width;
    std::int64_t position = 0;
    for (int x = 0; x < width; ++x, position += step)
    {
        const int left = static_cast<int>(position >> 16);
        const int right = std::min(left + 1, source_width - 1);
        const int sum = (top[left] >> shift) + (top[right] >> shift) + (bottom[left] >> shift) +
                        (bottom[right] >> shift);
        const int y = f.luma * (((sum + round) >> (2 + extra)) - f.black) + 128;
        const int pair = left & ~1;
        const int u = (chroma[pair] >> (shift + extra)) - 128;
        const int v = (chroma[pair + 1] >> (shift + extra)) - 128;
        out[4 * x] = clamped((y + f.red_v * v) >> 8);
        out[4 * x + 1] = clamped((y - f.green_u * u - f.green_v * v) >> 8);
        out[4 * x + 2] = clamped((y + f.blue_u * u) >> 8);
        out[4 * x + 3] = 255;
    }
}

template <class Sample>
void convert(const YuvPicture &picture, const Factors &f, std::uint8_t *rgba, int width, int height)
{
    const bool half = picture.width == 2 * width && picture.height == 2 * height;
    const std::int64_t step = (static_cast<std::int64_t>(picture.height) << 16) / height;
    std::int64_t position = 0;
    for (int row = 0; row < height; ++row, position += step)
    {
        const int line = static_cast<int>(position >> 16);
        const int below = std::min(line + 1, picture.height - 1);
        const auto *top = reinterpret_cast<const Sample *>(
            picture.luma + static_cast<std::size_t>(line) * picture.pitch);
        const auto *bottom = reinterpret_cast<const Sample *>(
            picture.luma + static_cast<std::size_t>(below) * picture.pitch);
        const auto *chroma = reinterpret_cast<const Sample *>(
            picture.chroma + static_cast<std::size_t>(line / 2) * picture.pitch);
        std::uint8_t *out = rgba + static_cast<std::size_t>(row) * width * 4;
        if (half)
            half_row(top, bottom, chroma, picture.shift, f, out, width);
        else
            any_row(top, bottom, chroma, picture.shift, f, out, width, picture.width);
    }
}

} // namespace

void yuv_to_rgba(const YuvPicture &picture, YuvMatrix matrix, bool full_range, std::uint8_t *rgba,
                 int width, int height)
{
    if (width <= 0 || height <= 0 || picture.width <= 0 || picture.height <= 0)
        return;
    const Factors f = factors(matrix, full_range);
    if (picture.wide)
        convert<std::uint16_t>(picture, f, rgba, width, height);
    else
        convert<std::uint8_t>(picture, f, rgba, width, height);
}

int ten_bit_shift(const std::uint16_t *samples, std::size_t count)
{
    unsigned seen = 0;
    for (std::size_t i = 0; i < count; ++i)
        seen |= samples[i];
    return (seen & 0xfc00u) != 0 && (seen & 0x003fu) == 0 ? 6 : 0;
}

} // namespace hui::media
