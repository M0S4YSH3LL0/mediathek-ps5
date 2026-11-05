// Mediathek - An HDR10 frame: the interface drawn over the film's picture in PQ.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <GL/glcorearb.h>

#include <cstdint>

namespace hui::gfx
{

// The film's picture as the composer takes it: where it sits on the virtual canvas
// (1920 x 1080) and whether its texture holds PQ codes with BT.2020 primaries (else an SDR
// picture, which is shown at the interface's white).
struct HdrPicture
{
    std::uint32_t texture = 0;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float opacity = 1.0f;
    bool pq = false;
};

// While the screen is HDR10, everything but the film is drawn as usual into an 8-bit layer
// of its own; finish() puts that layer over the film in PQ (its white at 203 nits, BT.709
// into BT.2020, blended so a half-transparent shade darkens the film as much as on an SDR
// screen, as VLC's PS5 port does) and writes each pixel's 10:10:10:2 word into the 8-bit
// channels of the target. The renderer stays as it is: one way to draw, one more pass.
class HdrComposer
{
  public:
    HdrComposer() = default;
    ~HdrComposer();
    HdrComposer(const HdrComposer &) = delete;
    HdrComposer &operator=(const HdrComposer &) = delete;

    // Makes the interface layer width x height and clears it to transparent. Draw the frame
    // into layer() next. False when it cannot be made.
    bool begin(int width, int height);
    GLuint layer() const
    {
        return framebuffer_;
    }
    // Draws the layer over `picture` into `target` (0 is the screen) as HDR10 words. Leaves
    // the GL state as it found it.
    void finish(GLuint target, const HdrPicture &picture);
    void release();

  private:
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint framebuffer_ = 0;
    GLuint texture_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool failed_ = false;
};

} // namespace hui::gfx
