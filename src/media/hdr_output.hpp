// Mediathek - HDR10 output: the PQ curve, the 10-bit words the screen reads, and when to switch.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace hui::media
{

// BT.2408's reference white: where the interface's white sits on an HDR screen, and what
// HDR calls paper white.
constexpr float kHdrWhiteNits = 203.0f;

// SMPTE ST 2084 (PQ): light in nits to its code (0..1) and back.
float pq_from_nits(float nits);
float nits_from_pq(float code);

// One pixel as the console's HDR10 scan-out reads it (pixel format 0x8100070400000000, the
// one VLC's PS5 port shows HDR10 with): A2R10G10B10 in a 32-bit word, blue in the low bits,
// PQ codes 0..1023 with BT.2020 primaries. The 8-bit screen keeps its bytes as blue, green,
// red, alpha, so a GL red channel holds bits 16-23 of the word.
std::uint32_t pack_hdr10(int red, int green, int blue);
struct Hdr10Pixel
{
    int red = 0;
    int green = 0;
    int blue = 0;
};
Hdr10Pixel unpack_hdr10(std::uint32_t word);

// The word an 8-bit screen pixel's channels make, read back as GL gives them (R, G, B, A).
std::uint32_t hdr10_word(const std::uint8_t *rgba);

// What an HDR10 frame looks like on an ordinary screen, for the PC preview: the interface's
// white (kHdrWhiteNits) as white, BT.2020 into BT.709, gamma 2.4. `pixels` are read-back RGBA
// pixels holding HDR10 words; they are rewritten in place as viewable RGBA.
void hdr10_to_viewable(std::uint8_t *pixels, std::size_t count);

// When the screen goes HDR and back. A wish has to hold a moment first: while a film starts its
// picture comes and goes, and every switch can blank the TV (VLC's port saw on, off, on, off in
// a second). A switch the console refused is not asked for again until the wish changes.
class HdrSwitch
{
  public:
    static constexpr double kHoldOn = 0.4;
    static constexpr double kHoldOff = 1.0;

    // Once a frame: whether HDR output is wanted, and the time in seconds. The change to make
    // now, if any; report what came of it with done().
    std::optional<bool> update(bool want, double seconds);
    void done(bool on, bool ok, double seconds);
    bool on() const
    {
        return on_;
    }

  private:
    bool on_ = false;
    bool wish_ = false;
    bool refused_ = false;
    double since_ = 0.0;
};

} // namespace hui::media
