// Mediathek - Subtitles from inside a film: their words, their pictures and when they show.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hui::media
{

// The words of an ASS event, which is what FFmpeg's decoders make of every text subtitle (SubRip,
// ASS, mov_text, WebVTT): "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text", or a
// whole "Dialogue:" line. The Text field without its override tags ("{\i1}") and drawings, with
// its line breaks ("\N", "\n") as '\n' and hard spaces ("\h") as spaces. Empty lines are dropped.
std::string ass_text(std::string_view event);

// What a track's title says that its flags may not: release groups name a forced track "Forced"
// and one for the hard of hearing "SDH" more often than they set the container's flags.
struct TitleFlags
{
    bool forced = false;
    bool hearing_impaired = false;
};
TitleFlags subtitle_title_flags(std::string_view title);

// One rectangle of a picture subtitle (PGS, VobSub, DVB) as FFmpeg decodes it: an index per
// pixel into a palette of 0xAARRGGBB colours with straight alpha.
struct PaletteRect
{
    int x = 0; // on the canvas the subtitle was made for
    int y = 0;
    int width = 0;
    int height = 0;
    const std::uint8_t *indices = nullptr;
    int stride = 0;
    const std::uint32_t *palette = nullptr;
    int colours = 0;
};

// A picture subtitle ready for a texture: RGBA with straight alpha, as the renderer blends, and
// where it sits as fractions of the picture it was made for, so it lands on the film wherever
// and however large the film is drawn.
struct SubtitleBitmap
{
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// Paints the rectangles into one bitmap just large enough for all of them, clipped to the
// canvas. False, with `out` emptied, when nothing in them would show.
bool compose_subtitle(const PaletteRect *rects, std::size_t count, int canvas_width,
                      int canvas_height, SubtitleBitmap *out);

// One decoded subtitle of the chosen track.
struct TimedSubtitle
{
    double start = 0.0; // film seconds
    // Film seconds; negative when the subtitle says nothing about its end, as PGS never does:
    // it then lasts until the next one starts, which may be one that only clears the screen.
    double end = -1.0;
    std::string text;      // a text subtitle's lines joined with '\n'
    SubtitleBitmap bitmap; // a picture subtitle; empty for text and for a clear
    std::uint64_t id = 0;  // given by SubtitleTimeline::add, so a picture is uploaded once
};

// The decoded subtitles of one track, ahead of and at the film's time.
class SubtitleTimeline
{
  public:
    void add(TimedSubtitle subtitle);
    void clear();
    // Drops everything that has ended by `seconds`.
    void forget_before(double seconds);
    // The lines of every text subtitle showing at `seconds`, "" for none.
    std::string text_at(double seconds) const;
    // The picture subtitle showing at `seconds` (the last to start, if several), or null.
    const TimedSubtitle *picture_at(double seconds) const;
    std::size_t size() const
    {
        return items_.size();
    }

  private:
    double end_of(std::size_t index) const;
    bool showing(std::size_t index, double seconds) const;

    std::vector<TimedSubtitle> items_; // ordered by start
    std::uint64_t next_id_ = 1;
};

} // namespace hui::media
