// Mediathek - HDR10 output: the PQ curve, the 10-bit words the screen reads, and when to switch.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/hdr_output.hpp"

#include <algorithm>
#include <cmath>

namespace hui::media
{

namespace
{

constexpr float kM1 = 0.1593017578125f;
constexpr float kM2 = 78.84375f;
constexpr float kC1 = 0.8359375f;
constexpr float kC2 = 18.8515625f;
constexpr float kC3 = 18.6875f;

int ten_bits(int value)
{
    return std::clamp(value, 0, 1023);
}

std::uint8_t gamma_byte(float light)
{
    const float code = std::pow(std::clamp(light, 0.0f, 1.0f), 1.0f / 2.4f);
    return static_cast<std::uint8_t>(code * 255.0f + 0.5f);
}

} // namespace

float pq_from_nits(float nits)
{
    const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), kM1);
    return std::pow((kC1 + kC2 * y) / (1.0f + kC3 * y), kM2);
}

float nits_from_pq(float code)
{
    const float p = std::pow(std::clamp(code, 0.0f, 1.0f), 1.0f / kM2);
    return 10000.0f * std::pow(std::max(p - kC1, 0.0f) / (kC2 - kC3 * p), 1.0f / kM1);
}

std::uint32_t pack_hdr10(int red, int green, int blue)
{
    return static_cast<std::uint32_t>(ten_bits(blue)) |
           (static_cast<std::uint32_t>(ten_bits(green)) << 10) |
           (static_cast<std::uint32_t>(ten_bits(red)) << 20) | (3u << 30);
}

Hdr10Pixel unpack_hdr10(std::uint32_t word)
{
    Hdr10Pixel pixel;
    pixel.blue = static_cast<int>(word & 1023u);
    pixel.green = static_cast<int>((word >> 10) & 1023u);
    pixel.red = static_cast<int>((word >> 20) & 1023u);
    return pixel;
}

std::uint32_t hdr10_word(const std::uint8_t *rgba)
{
    return static_cast<std::uint32_t>(rgba[2]) | (static_cast<std::uint32_t>(rgba[1]) << 8) |
           (static_cast<std::uint32_t>(rgba[0]) << 16) |
           (static_cast<std::uint32_t>(rgba[3]) << 24);
}

void hdr10_to_viewable(std::uint8_t *pixels, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        std::uint8_t *px = pixels + i * 4;
        const Hdr10Pixel code = unpack_hdr10(hdr10_word(px));
        const float r = nits_from_pq(static_cast<float>(code.red) / 1023.0f) / kHdrWhiteNits;
        const float g = nits_from_pq(static_cast<float>(code.green) / 1023.0f) / kHdrWhiteNits;
        const float b = nits_from_pq(static_cast<float>(code.blue) / 1023.0f) / kHdrWhiteNits;
        // BT.2020 into BT.709.
        px[0] = gamma_byte(1.6605f * r - 0.5876f * g - 0.0728f * b);
        px[1] = gamma_byte(-0.1246f * r + 1.1329f * g - 0.0083f * b);
        px[2] = gamma_byte(-0.0182f * r - 0.1006f * g + 1.1187f * b);
        px[3] = 255;
    }
}

std::optional<bool> HdrSwitch::update(bool want, double seconds)
{
    if (want != wish_)
    {
        wish_ = want;
        since_ = seconds;
    }
    if (!want)
        refused_ = false;
    const double hold = want ? kHoldOn : kHoldOff;
    if (want != on_ && !(want && refused_) && seconds - since_ >= hold)
        return want;
    return std::nullopt;
}

void HdrSwitch::done(bool on, bool ok, double seconds)
{
    if (ok)
    {
        on_ = on;
        return;
    }
    if (on)
        refused_ = true;
    else
        since_ = seconds; // the screen stays HDR: asked again after another hold, not every frame
}

} // namespace hui::media
