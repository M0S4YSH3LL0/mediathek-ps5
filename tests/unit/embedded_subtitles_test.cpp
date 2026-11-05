// Mediathek - Tests for the subtitles inside a film: words, pictures and timing.
// Copyright (C) 2026 BlackBearReloaded SPDX-License-Identifier: GPL-3.0-or-later

#include "media/embedded_subtitles.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace
{

using namespace hui::media;

TEST(AssText, TakesTheTextFieldOfAnEventAsFfmpegWritesIt)
{
    EXPECT_EQ(ass_text("0,0,Default,,0,0,0,,Where were you?"), "Where were you?");
    // Commas in the text belong to it.
    EXPECT_EQ(ass_text("3,0,Default,,0,0,0,,Late, as always."), "Late, as always.");
    // An old-style whole line has one field more.
    EXPECT_EQ(ass_text("Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,Hello"), "Hello");
    EXPECT_EQ(ass_text("no fields at all"), "");
}

TEST(AssText, DropsStylingAndKeepsTheLineBreaks)
{
    EXPECT_EQ(ass_text("0,0,Default,,0,0,0,,{\\i1}Quiet{\\i0} now.\\NBoth of you."),
              "Quiet now.\nBoth of you.");
    EXPECT_EQ(ass_text("0,0,Default,,0,0,0,,One\\nTwo\\hThree"), "One\nTwo Three");
    // A drawing is shapes, not words; the text after it is kept.
    EXPECT_EQ(ass_text("0,0,Sign,,0,0,0,,{\\p1}m 0 0 l 100 0 100 100{\\p0}Exit"), "Exit");
    // Lines left empty by the tags go, and so do the spaces around them.
    EXPECT_EQ(ass_text("0,0,Default,,0,0,0,, {\\an8} \\NOnly this "), "Only this");
}

TEST(SubtitleTitleFlags, ReadsForcedAndSdhFromTheTitle)
{
    EXPECT_TRUE(subtitle_title_flags("German Forced").forced);
    EXPECT_TRUE(subtitle_title_flags("Deutsch (erzwungen)").forced);
    EXPECT_FALSE(subtitle_title_flags("German").forced);
    EXPECT_TRUE(subtitle_title_flags("English SDH").hearing_impaired);
    EXPECT_TRUE(subtitle_title_flags("English [CC]").hearing_impaired);
    EXPECT_FALSE(subtitle_title_flags("Accent").hearing_impaired) << "a word, not letters";
    EXPECT_FALSE(subtitle_title_flags("").forced);
}

TEST(ComposeSubtitle, PaintsThePaletteIntoABitmapJustLargeEnough)
{
    // Two rows of four pixels: clear, red, half-transparent white, an index beyond the palette.
    const std::uint8_t indices[] = {0, 1, 2, 9, //
                                    1, 1, 1, 1};
    const std::uint32_t palette[] = {0x00000000u, 0xffff0000u, 0x80ffffffu};
    PaletteRect rect;
    rect.x = 100;
    rect.y = 50;
    rect.width = 4;
    rect.height = 2;
    rect.indices = indices;
    rect.stride = 4;
    rect.palette = palette;
    rect.colours = 3;
    SubtitleBitmap bitmap;
    ASSERT_TRUE(compose_subtitle(&rect, 1, 200, 100, &bitmap));
    EXPECT_EQ(bitmap.width, 4);
    EXPECT_EQ(bitmap.height, 2);
    EXPECT_FLOAT_EQ(bitmap.x, 0.5f);
    EXPECT_FLOAT_EQ(bitmap.y, 0.5f);
    EXPECT_FLOAT_EQ(bitmap.w, 0.02f);
    EXPECT_FLOAT_EQ(bitmap.h, 0.02f);
    const std::vector<std::uint8_t> &p = bitmap.rgba;
    EXPECT_EQ(p[3], 0) << "index 0 is clear";
    EXPECT_EQ(p[4], 255);
    EXPECT_EQ(p[5], 0);
    EXPECT_EQ(p[7], 255) << "red, opaque";
    EXPECT_EQ(p[8], 255);
    EXPECT_EQ(p[11], 0x80) << "straight alpha: the colour stays white";
    EXPECT_EQ(p[15], 0) << "beyond the palette: clear";
}

TEST(ComposeSubtitle, CoversEveryRectangleAndClipsToTheCanvas)
{
    const std::uint8_t one[] = {1};
    const std::uint32_t palette[] = {0, 0xffffffffu};
    PaletteRect rects[2];
    rects[0].x = 10;
    rects[0].y = 80;
    rects[1].x = 30;
    rects[1].y = 99; // its second row is off the canvas
    for (PaletteRect &r : rects)
    {
        r.width = 1;
        r.height = r.y == 99 ? 2 : 1;
        r.indices = one;
        r.stride = 0; // every row the same pixel
        r.palette = palette;
        r.colours = 2;
    }
    SubtitleBitmap bitmap;
    ASSERT_TRUE(compose_subtitle(rects, 2, 100, 100, &bitmap));
    EXPECT_EQ(bitmap.width, 21);
    EXPECT_EQ(bitmap.height, 20);
    EXPECT_EQ(bitmap.rgba[3], 255);
    EXPECT_EQ(bitmap.rgba[(19 * 21 + 20) * 4 + 3], 255);
    EXPECT_EQ(bitmap.rgba[(10 * 21 + 10) * 4 + 3], 0) << "between them: clear";
}

TEST(ComposeSubtitle, NothingVisibleIsNoPicture)
{
    const std::uint8_t clear[] = {0, 0};
    const std::uint32_t palette[] = {0x00ffffffu};
    PaletteRect rect;
    rect.width = 2;
    rect.height = 1;
    rect.indices = clear;
    rect.stride = 2;
    rect.palette = palette;
    rect.colours = 1;
    SubtitleBitmap bitmap;
    EXPECT_FALSE(compose_subtitle(&rect, 1, 100, 100, &bitmap));
    EXPECT_TRUE(bitmap.rgba.empty());
    EXPECT_FALSE(compose_subtitle(&rect, 1, 0, 0, &bitmap)) << "no canvas";
    EXPECT_FALSE(compose_subtitle(nullptr, 0, 100, 100, &bitmap));
}

TimedSubtitle text(double start, double end, const char *words)
{
    TimedSubtitle s;
    s.start = start;
    s.end = end;
    s.text = words;
    return s;
}

TimedSubtitle picture(double start, double end = -1.0)
{
    TimedSubtitle s;
    s.start = start;
    s.end = end;
    s.bitmap.rgba = {255, 255, 255, 255};
    s.bitmap.width = 1;
    s.bitmap.height = 1;
    return s;
}

TEST(SubtitleTimeline, TextShowsFromItsStartUntilItsEndAndOverlapsJoin)
{
    SubtitleTimeline timeline;
    timeline.add(text(1.0, 3.0, "First"));
    timeline.add(text(2.0, 4.0, "Second"));
    EXPECT_EQ(timeline.text_at(0.5), "");
    EXPECT_EQ(timeline.text_at(1.5), "First");
    EXPECT_EQ(timeline.text_at(2.5), "First\nSecond");
    EXPECT_EQ(timeline.text_at(3.0), "Second") << "the end is not shown";
    timeline.forget_before(3.5);
    EXPECT_EQ(timeline.size(), 1u);
    timeline.forget_before(4.0);
    EXPECT_EQ(timeline.size(), 0u);
}

// PGS never says when a picture ends: the next display set does, often one that is empty.
TEST(SubtitleTimeline, APictureWithoutAnEndLastsUntilTheNextOne)
{
    SubtitleTimeline timeline;
    timeline.add(picture(1.0));
    EXPECT_NE(timeline.picture_at(50.0), nullptr) << "nothing has ended it yet";
    TimedSubtitle clear;
    clear.start = 4.0;
    timeline.add(clear);
    timeline.add(picture(6.0));
    EXPECT_EQ(timeline.picture_at(0.5), nullptr);
    const TimedSubtitle *first = timeline.picture_at(2.0);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(timeline.picture_at(4.5), nullptr) << "cleared";
    const TimedSubtitle *second = timeline.picture_at(6.5);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first->id, second->id) << "a new picture to upload";
    timeline.forget_before(6.5);
    EXPECT_EQ(timeline.size(), 1u) << "the first and the clear have ended";
}

TEST(SubtitleTimeline, ArrivingOutOfOrderStillShowsInOrder)
{
    SubtitleTimeline timeline;
    timeline.add(picture(5.0));
    timeline.add(picture(2.0));
    const TimedSubtitle *early = timeline.picture_at(3.0);
    ASSERT_NE(early, nullptr);
    EXPECT_DOUBLE_EQ(early->start, 2.0) << "ended by the one at 5, which came first";
    timeline.clear();
    EXPECT_EQ(timeline.picture_at(6.0), nullptr);
}

} // namespace
