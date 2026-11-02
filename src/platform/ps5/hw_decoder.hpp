// Mediathek - The console's hardware picture decoder (libSceVideodec2).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/hw_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace hui::ps5
{

// A decoded picture, in the decoder's own memory. It stays good for the next
// HwDecoder::kPictureLife calls of decode(); the player keeps far fewer waiting.
struct HwPicture
{
    const std::uint8_t *luma = nullptr;
    const std::uint8_t *chroma = nullptr;
    std::size_t pitch = 0;
    int width = 0; // coded: can be more than the film shows
    int height = 0;
    bool wide = false; // 16-bit samples
};

class HwDecoder
{
  public:
    enum class Result
    {
        nothing, // taken; the picture comes later
        picture,
        refused
    };

    static constexpr int kPictureLife = 8;

    // Null when the console does not give it (the reason is in the log). Its memory is
    // not the app's heap: direct memory for the pictures, flexible for its own work.
    static std::unique_ptr<HwDecoder> open(const media::HwMode &mode);
    ~HwDecoder();
    HwDecoder(const HwDecoder &) = delete;
    HwDecoder &operator=(const HwDecoder &) = delete;

    // Where the next access unit goes, `input_capacity()` bytes of it. A fixed place
    // because the decoder reads it from memory the graphics chip can see.
    std::uint8_t *next_input();
    static std::size_t input_capacity();

    // Submits what next_input() was given.
    Result decode(std::size_t size, HwPicture *picture);
    // Gets what the decoder still holds at the end of the file; `nothing` when empty.
    Result drain(HwPicture *picture);

    // Starts over for a seek: resetting a running decoder leaves some 4K streams
    // unable to restart (the next key frame is refused, and each one after it), a new
    // one on the same memory takes about 15 ms. False: there is no decoder now.
    bool renew();

  private:
    struct Parts;
    explicit HwDecoder(std::unique_ptr<Parts> parts);
    Result finish(int code, const void *frame, const void *output, HwPicture *picture);

    std::unique_ptr<Parts> parts_;
};

} // namespace hui::ps5
