// Mediathek - What the console's hardware picture decoder takes and how it is fed.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hui::media
{

enum class HwCodec
{
    h264,
    hevc
};

// One way of setting up the decoder: everything its configuration asks about the film.
struct HwMode
{
    HwCodec codec = HwCodec::h264;
    unsigned profile = 0; // the codec's own number: H.264 100 High, HEVC 1 Main, 2 Main10
    unsigned level = 0;
    unsigned fallback_level = 0; // tried when the decoder refuses `level`; 0 for none
    int max_width = 0;           // what the decoder is sized for, not what the film shows
    int max_height = 0;
};

// Films up to 1080p stay on FFmpeg: it plays them well, on the app's own memory.
bool wants_hw_decoder(int width, int height);

// The mode for a film, or nothing when the hardware cannot play it. `profile` is
// the number FFmpeg reports (negative: it does not know), `depth` the bits per sample.
std::optional<HwMode> hw_mode(HwCodec codec, int profile, int depth, bool yuv420, int width,
                              int height);

// The type of the first picture NAL unit in an HEVC access unit in start-code
// form, or -1 if it holds no picture.
int hevc_picture_type(const std::uint8_t *unit, std::size_t size);

// Copies an HEVC access unit without what the decoder refuses: NAL units of types
// 48-63, which a decoder must ignore and which carry Dolby Vision's extra layers, NAL
// units of any layer but the base one (Dolby Vision profile 7's enhancement layer:
// the decoder refused it at its first unit in VLC's port), and a parameter set that a later one in
// the same unit replaces (a remux can hold two different PPS 0, one from the file's header and one
// from the stream, and the decoder then refuses the key frame). The output is never longer than the
// input.
std::size_t clean_hevc_unit(const std::uint8_t *unit, std::size_t size, std::uint8_t *out);

// True when an H.264 access unit in start-code form can start a decoder: an IDR picture,
// or a picture of I slices only (judged by its first slice, as VLC's port does).
bool h264_is_key(const std::uint8_t *unit, std::size_t size);

// Decides which HEVC access units may reach the decoder after a random access.
class HevcEntry
{
  public:
    // The stream starts here (the film's start, a seek, a jump): a key frame in an
    // open GOP is followed by RASL pictures predicted from what came before it, which
    // was never decoded and which the decoder reports as errors, one per picture.
    void restart();

    // True when an access unit whose first picture has this type may be submitted.
    bool admit(int type);

    // The decoder refused the key frame just submitted: everything up to the next
    // one is predicted from what it does not have, and would show as coloured blocks.
    void refused();

  private:
    bool leading_ = true;
    bool need_key_ = false;
};

// The decoder gives pictures back in display order with no time on them. It took the
// access units in decode order with theirs, so each picture's is the earliest still out.
class PendingStamps
{
  public:
    void push(double seconds);
    bool take(double *seconds); // false when none is out
    void reset();

  private:
    std::vector<double> stamps_;
};

} // namespace hui::media
