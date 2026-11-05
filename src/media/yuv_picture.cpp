// Mediathek - A decoded two-plane picture made into the RGBA picture the screen shows.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/yuv_picture.hpp"

#include <algorithm>
#include <cstring>

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
             const Factors &f, std::uint8_t *out, int width, int source_width, bool same)
{
    constexpr int extra = Reading<Sample>::kExtra;
    constexpr int round = 2 << extra;
    const std::int64_t step = (static_cast<std::int64_t>(source_width) << 16) / width;
    std::int64_t position = 0;
    for (int x = 0; x < width; ++x, position += step)
    {
        const int left = static_cast<int>(position >> 16);
        const int right = same ? left : std::min(left + 1, source_width - 1);
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
    // At its own size each pixel is its own sample: averaging it with the next ones would
    // soften a 1080p film by half a pixel, and the hardware decoder now gives those too.
    const bool same = picture.width == width && picture.height == height;
    const std::int64_t step = (static_cast<std::int64_t>(picture.height) << 16) / height;
    std::int64_t position = 0;
    for (int row = 0; row < height; ++row, position += step)
    {
        const int line = static_cast<int>(position >> 16);
        const int below = same ? line : std::min(line + 1, picture.height - 1);
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
            any_row(top, bottom, chroma, picture.shift, f, out, width, picture.width, same);
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

float hdr_peak_nits(float max_content_light, float mastering_peak)
{
    // Below 400 a curve made for HDR would darken what is meant to be bright; nothing is
    // mastered beyond 10 000, the end of the PQ scale.
    constexpr float kLowest = 400.0f;
    constexpr float kHighest = 10000.0f;
    constexpr float kUsual = 1000.0f;
    float peak = kUsual;
    if (max_content_light > 0.0f)
        peak = max_content_light;
    else if (mastering_peak > 0.0f)
        peak = mastering_peak;
    return std::clamp(peak, kLowest, kHighest);
}

YuvLayout yuv_layout(const YuvPicture &source, int width, int height)
{
    YuvLayout layout;
    if (source.width <= 0 || source.height <= 0)
        return layout;
    const bool halve =
        width > 0 && height > 0 && source.width >= 2 * width && source.height >= 2 * height;
    layout.luma_width = halve ? source.width / 2 : source.width;
    layout.luma_height = halve ? source.height / 2 : source.height;
    layout.chroma_width = (source.width + 1) / 2;
    layout.chroma_height = (source.height + 1) / 2;
    layout.wide = source.wide;
    return layout;
}

std::size_t yuv_luma_bytes(const YuvLayout &layout)
{
    const std::size_t sample = layout.wide ? 2 : 1;
    return static_cast<std::size_t>(layout.luma_width) *
           static_cast<std::size_t>(layout.luma_height) * sample;
}

std::size_t yuv_bytes(const YuvLayout &layout)
{
    const std::size_t sample = layout.wide ? 2 : 1;
    return yuv_luma_bytes(layout) + static_cast<std::size_t>(layout.chroma_width) *
                                        static_cast<std::size_t>(layout.chroma_height) * 2 * sample;
}

namespace
{

// 10 bits wherever they sit, moved to the top of the word.
inline std::uint16_t raised(std::uint16_t sample, int shift)
{
    return static_cast<std::uint16_t>((static_cast<unsigned>(sample) >> shift << 6) & 0xffc0u);
}

template <class Sample> Sample stored(unsigned value, int shift)
{
    if constexpr (sizeof(Sample) == 1)
    {
        (void)shift;
        return static_cast<Sample>(value);
    }
    else
    {
        return raised(static_cast<std::uint16_t>(value), shift);
    }
}

template <class Sample>
void copy_luma(const YuvPicture &source, const YuvLayout &layout, Sample *out)
{
    const bool halve = layout.luma_width < source.width;
    for (int row = 0; row < layout.luma_height; ++row)
    {
        Sample *line = out + static_cast<std::size_t>(row) * layout.luma_width;
        if (!halve)
        {
            const auto *in = reinterpret_cast<const Sample *>(
                source.luma + static_cast<std::size_t>(row) * source.pitch);
            for (int x = 0; x < layout.luma_width; ++x)
                line[x] = stored<Sample>(in[x], source.shift);
            continue;
        }
        const auto *top = reinterpret_cast<const Sample *>(
            source.luma + static_cast<std::size_t>(2 * row) * source.pitch);
        const auto *bottom = reinterpret_cast<const Sample *>(
            source.luma + static_cast<std::size_t>(2 * row + 1) * source.pitch);
        for (int x = 0; x < layout.luma_width; ++x)
        {
            const unsigned sum = static_cast<unsigned>(top[2 * x]) + top[2 * x + 1] +
                                 bottom[2 * x] + bottom[2 * x + 1];
            line[x] = stored<Sample>((sum + 2) >> 2, source.shift);
        }
    }
}

template <class Sample>
void copy_chroma(const YuvPicture &source, const YuvLayout &layout, Sample *out)
{
    const int samples = 2 * layout.chroma_width;
    for (int row = 0; row < layout.chroma_height; ++row)
    {
        Sample *line = out + static_cast<std::size_t>(row) * samples;
        if (source.chroma_v != nullptr)
        {
            const std::size_t offset = static_cast<std::size_t>(row) * source.chroma_pitch;
            const auto *u = reinterpret_cast<const Sample *>(source.chroma + offset);
            const auto *v = reinterpret_cast<const Sample *>(source.chroma_v + offset);
            for (int x = 0; x < layout.chroma_width; ++x)
            {
                line[2 * x] = stored<Sample>(u[x], source.shift);
                line[2 * x + 1] = stored<Sample>(v[x], source.shift);
            }
            continue;
        }
        const auto *in = reinterpret_cast<const Sample *>(
            source.chroma + static_cast<std::size_t>(row) * source.pitch);
        if constexpr (sizeof(Sample) == 1)
            std::memcpy(line, in, static_cast<std::size_t>(samples));
        else
            for (int x = 0; x < samples; ++x)
                line[x] = raised(in[x], source.shift);
    }
}

} // namespace

void copy_planes(const YuvPicture &source, const YuvLayout &layout, std::uint8_t *out)
{
    if (layout.luma_width <= 0 || source.luma == nullptr || source.chroma == nullptr)
        return;
    std::uint8_t *chroma = out + yuv_luma_bytes(layout);
    if (layout.wide)
    {
        copy_luma(source, layout, reinterpret_cast<std::uint16_t *>(out));
        copy_chroma(source, layout, reinterpret_cast<std::uint16_t *>(chroma));
    }
    else
    {
        copy_luma(source, layout, out);
        copy_chroma(source, layout, chroma);
    }
}

int ten_bit_shift(const std::uint16_t *samples, std::size_t count)
{
    unsigned seen = 0;
    for (std::size_t i = 0; i < count; ++i)
        seen |= samples[i];
    return (seen & 0xfc00u) != 0 && (seen & 0x003fu) == 0 ? 6 : 0;
}

} // namespace hui::media
