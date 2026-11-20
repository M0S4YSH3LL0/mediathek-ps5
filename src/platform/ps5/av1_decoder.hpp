// Mediathek - AV1 pictures from dav1d, for the films PacBrew's FFmpeg cannot decode.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/picture_pool.hpp"
#include "media/yuv_picture.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hui::ps5
{

// How a picture's chroma is sampled. AV1's Main profile, which nearly every film uses, is
// 4:2:0 (or 4:0:0, luma only); the others come from the High and Professional profiles.
enum class Av1Layout : std::uint8_t
{
    i400,
    i420,
    i422,
    i444
};

// A decoded picture, valid only while the sink that was handed it runs. Its three planes
// are `planes.luma`, `planes.chroma` (U) and `planes.chroma_v` (V); `planes.wide` is set
// for 10 and 12 bits, which sit at the bottom of 16-bit words.
struct Av1Picture
{
    media::YuvPicture planes;
    Av1Layout layout = Av1Layout::i420;
    int bits = 8;
    std::int64_t timestamp = 0; // as handed in with its data
    media::CodedColour colour;
};

struct Av1Settings
{
    int threads = 0;         // 0: as many as the system says it has
    int frame_delay = 0;     // frames decoded at once; 0: dav1d's choice for `threads`
    unsigned max_pixels = 0; // a bigger picture is refused rather than decoded; 0: any
    // Where the pictures' memory comes from; null: dav1d's own allocations (the heap).
    media::PicturePool *pool = nullptr;
    std::function<void(const char *line)> log; // dav1d's own messages, one line each
};

// dav1d's decoder, used from one thread: it runs threads of its own behind these calls.
class Av1Decoder
{
  public:
    // Returns false to stop handing out pictures (the player is leaving the film).
    using Sink = std::function<bool(const Av1Picture &)>;

    // Null when dav1d would not start; `error` gets its code.
    static std::unique_ptr<Av1Decoder> open(const Av1Settings &settings, int *error);
    ~Av1Decoder();
    Av1Decoder(const Av1Decoder &) = delete;
    Av1Decoder &operator=(const Av1Decoder &) = delete;

    // Decodes one temporal unit (one packet of the file) and hands `sink` any picture that is
    // ready: with frame threads that is an earlier one, the pictures come out a few units
    // behind. Returns 0, or dav1d's negative error for this unit, which is then dropped.
    int decode(const std::uint8_t *data, std::size_t size, std::int64_t timestamp,
               const Sink &sink);
    // At the end of the file: the pictures dav1d still holds.
    void drain(const Sink &sink);
    // Forgets everything for a seek: the next unit should start with a key frame.
    void flush();

    static std::string version();

  private:
    struct Parts;
    explicit Av1Decoder(std::unique_ptr<Parts> parts);
    std::unique_ptr<Parts> parts_;
};

} // namespace hui::ps5
