// Mediathek - Turns a video picture's YUV planes into the player's RGBA texture on the GPU.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/yuv_picture.hpp"

#include <GL/glcorearb.h>

#include <cstdint>

namespace hui::gfx
{

// The planes go up as one- and two-channel textures and a shader draws them into the
// texture the screens already show, so a picture still reaches the screen the one way
// everything does. The shader also does what the CPU has no time for at 4K: HDR's
// light, PQ or HLG, brought down to an ordinary screen's range (tone mapping) and
// BT.2020's colours into BT.709's. While the screen is HDR10 an HDR picture is left as it
// is instead (HLG turned into PQ), in a 10-bit texture, for gfx::HdrComposer.
class VideoConverter
{
  public:
    VideoConverter() = default;
    ~VideoConverter();
    VideoConverter(const VideoConverter &) = delete;
    VideoConverter &operator=(const VideoConverter &) = delete;

    // Draws `frame` into `target`, an RGBA8 texture of width x height. With `pq_output` an
    // HDR picture goes in as PQ codes with BT.2020 primaries, and the texture becomes
    // RGB10_A2 for it (RGBA8 again for the next SDR one). Leaves the GL state as it found
    // it. False when the shader or the target cannot be made: the caller converts on the
    // CPU from then on.
    bool convert(const media::YuvFrame &frame, std::uint32_t target, int width, int height,
                 bool pq_output = false);
    void release();

    // What the last picture drawn into `texture` was: HDR (PQ or HLG), and left as PQ codes.
    bool shows_hdr(std::uint32_t texture) const
    {
        return texture != 0 && texture == last_target_ && last_hdr_;
    }
    bool holds_pq(std::uint32_t texture) const
    {
        return texture != 0 && texture == last_target_ && last_pq_;
    }

  private:
    bool init();
    void upload(const media::YuvFrame &frame);

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint framebuffer_ = 0;
    GLuint luma_ = 0;
    GLuint chroma_ = 0;
    media::YuvLayout layout_;
    bool failed_ = false;
    std::uint32_t last_target_ = 0;
    bool last_hdr_ = false;
    bool last_pq_ = false;
};

} // namespace hui::gfx
