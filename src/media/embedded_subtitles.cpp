// Mediathek - Subtitles from inside a film: their words, their pictures and when they show.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/embedded_subtitles.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>

namespace hui::media
{

namespace
{

// An ASS event as FFmpeg's decoders write it has eight fields before the text; a whole
// "Dialogue:" line, as older ones did, has nine (with Start and End, but no ReadOrder).
constexpr std::size_t kEventFields = 8;
constexpr std::size_t kDialogueFields = 9;
constexpr std::string_view kDialogue = "Dialogue:";
// More than any film shows at once, and what a seek leaves behind is cleared anyway: the cap
// only keeps a track whose subtitles never end from growing for the length of the film.
constexpr std::size_t kMaxItems = 256;

std::string_view after_fields(std::string_view event, std::size_t fields)
{
    std::size_t at = 0;
    for (std::size_t i = 0; i < fields; ++i)
    {
        const std::size_t comma = event.find(',', at);
        if (comma == std::string_view::npos)
            return {}; // not an event: no text in it
        at = comma + 1;
    }
    return event.substr(at);
}

// The value of a "\p" tag inside an override block: drawing mode is on while it is above 0.
// -1 when the block has none.
int drawing_mode(std::string_view block)
{
    int mode = -1;
    for (std::size_t i = 0; i + 1 < block.size(); ++i)
        if (block[i] == '\\' && block[i + 1] == 'p' && i + 2 < block.size() &&
            std::isdigit(static_cast<unsigned char>(block[i + 2])) != 0)
            mode = block[i + 2] - '0';
    return mode;
}

std::string trimmed(std::string_view line)
{
    std::size_t begin = 0;
    std::size_t end = line.size();
    while (begin < end && (line[begin] == ' ' || line[begin] == '\t' || line[begin] == '\r'))
        ++begin;
    while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r'))
        --end;
    return std::string(line.substr(begin, end - begin));
}

std::string lower_ascii(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool has_word(std::string_view text, std::string_view word)
{
    for (std::size_t at = text.find(word); at != std::string_view::npos;
         at = text.find(word, at + 1))
    {
        const bool left = at == 0 || std::isalpha(static_cast<unsigned char>(text[at - 1])) == 0;
        const std::size_t after = at + word.size();
        const bool right =
            after >= text.size() || std::isalpha(static_cast<unsigned char>(text[after])) == 0;
        if (left && right)
            return true;
    }
    return false;
}

} // namespace

std::string ass_text(std::string_view event)
{
    const std::string_view text = event.substr(0, kDialogue.size()) == kDialogue
                                      ? after_fields(event, kDialogueFields)
                                      : after_fields(event, kEventFields);
    std::string plain;
    plain.reserve(text.size());
    bool drawing = false;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '{')
        {
            const std::size_t close = text.find('}', i);
            if (close != std::string_view::npos)
            {
                const int mode = drawing_mode(text.substr(i, close - i));
                if (mode >= 0)
                    drawing = mode > 0;
                i = close;
                continue;
            }
        }
        if (c == '\\' && i + 1 < text.size())
        {
            const char next = text[i + 1];
            if (next == 'N' || next == 'n')
            {
                plain += '\n';
                ++i;
                continue;
            }
            if (next == 'h')
            {
                if (!drawing)
                    plain += ' ';
                ++i;
                continue;
            }
        }
        if (!drawing)
            plain += c;
    }
    std::string out;
    std::size_t start = 0;
    while (start <= plain.size())
    {
        std::size_t end = plain.find('\n', start);
        if (end == std::string::npos)
            end = plain.size();
        const std::string line = trimmed(std::string_view(plain).substr(start, end - start));
        if (!line.empty())
        {
            if (!out.empty())
                out += '\n';
            out += line;
        }
        start = end + 1;
    }
    return out;
}

TitleFlags subtitle_title_flags(std::string_view title)
{
    const std::string lower = lower_ascii(title);
    TitleFlags flags;
    flags.forced =
        lower.find("forced") != std::string::npos || lower.find("erzwungen") != std::string::npos;
    flags.hearing_impaired = has_word(lower, "sdh") || has_word(lower, "cc") ||
                             lower.find("hearing") != std::string::npos ||
                             lower.find("h\xC3\xB6rgesch\xC3\xA4"
                                        "digte") != std::string::npos;
    return flags;
}

bool compose_subtitle(const PaletteRect *rects, std::size_t count, int canvas_width,
                      int canvas_height, SubtitleBitmap *out)
{
    *out = SubtitleBitmap{};
    if (canvas_width <= 0 || canvas_height <= 0 || rects == nullptr)
        return false;
    // The box around every rectangle, inside the canvas.
    int left = canvas_width;
    int top = canvas_height;
    int right = 0;
    int bottom = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const PaletteRect &r = rects[i];
        if (r.width <= 0 || r.height <= 0 || r.indices == nullptr || r.palette == nullptr)
            continue;
        left = std::min(left, std::max(r.x, 0));
        top = std::min(top, std::max(r.y, 0));
        right = std::max(right, std::min(r.x + r.width, canvas_width));
        bottom = std::max(bottom, std::min(r.y + r.height, canvas_height));
    }
    if (right <= left || bottom <= top)
        return false;
    const int width = right - left;
    const int height = bottom - top;
    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    bool visible = false;
    for (std::size_t i = 0; i < count; ++i)
    {
        const PaletteRect &r = rects[i];
        if (r.width <= 0 || r.height <= 0 || r.indices == nullptr || r.palette == nullptr)
            continue;
        const int from_x = std::max(r.x, left);
        const int to_x = std::min(r.x + r.width, right);
        for (int y = std::max(r.y, top); y < std::min(r.y + r.height, bottom); ++y)
        {
            const std::uint8_t *row = r.indices + static_cast<std::ptrdiff_t>(y - r.y) * r.stride;
            std::uint8_t *dst = rgba.data() + (static_cast<std::size_t>(y - top) * width) * 4;
            for (int x = from_x; x < to_x; ++x)
            {
                const int index = row[x - r.x];
                if (index >= r.colours)
                    continue;
                const std::uint32_t colour = r.palette[index];
                const auto alpha = static_cast<std::uint8_t>(colour >> 24);
                if (alpha == 0)
                    continue; // a later rectangle's clear pixels do not punch holes
                std::uint8_t *px = dst + static_cast<std::size_t>(x - left) * 4;
                px[0] = static_cast<std::uint8_t>(colour >> 16);
                px[1] = static_cast<std::uint8_t>(colour >> 8);
                px[2] = static_cast<std::uint8_t>(colour);
                px[3] = alpha;
                visible = true;
            }
        }
    }
    if (!visible)
        return false;
    out->rgba = std::move(rgba);
    out->width = width;
    out->height = height;
    out->x = static_cast<float>(left) / static_cast<float>(canvas_width);
    out->y = static_cast<float>(top) / static_cast<float>(canvas_height);
    out->w = static_cast<float>(width) / static_cast<float>(canvas_width);
    out->h = static_cast<float>(height) / static_cast<float>(canvas_height);
    return true;
}

void SubtitleTimeline::add(TimedSubtitle subtitle)
{
    subtitle.id = next_id_++;
    // Usually last: subtitles arrive in the order they show.
    auto at = items_.end();
    while (at != items_.begin() && std::prev(at)->start > subtitle.start)
        --at;
    items_.insert(at, std::move(subtitle));
    if (items_.size() > kMaxItems)
        items_.erase(items_.begin());
}

void SubtitleTimeline::clear()
{
    items_.clear();
}

double SubtitleTimeline::end_of(std::size_t index) const
{
    const TimedSubtitle &item = items_[index];
    if (item.end >= 0.0)
        return item.end;
    for (std::size_t next = index + 1; next < items_.size(); ++next)
        if (items_[next].start > item.start)
            return items_[next].start;
    return std::numeric_limits<double>::infinity();
}

bool SubtitleTimeline::showing(std::size_t index, double seconds) const
{
    return items_[index].start <= seconds && seconds < end_of(index);
}

void SubtitleTimeline::forget_before(double seconds)
{
    // Ends depend only on what comes later, so deciding first and erasing after is safe.
    std::vector<bool> ended(items_.size());
    for (std::size_t i = 0; i < items_.size(); ++i)
        ended[i] = end_of(i) <= seconds;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < items_.size(); ++i)
        if (!ended[i])
        {
            if (kept != i)
                items_[kept] = std::move(items_[i]);
            ++kept;
        }
    items_.resize(kept);
}

std::string SubtitleTimeline::text_at(double seconds) const
{
    std::string out;
    for (std::size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].text.empty() || !showing(i, seconds))
            continue;
        if (!out.empty())
            out += '\n';
        out += items_[i].text;
    }
    return out;
}

const TimedSubtitle *SubtitleTimeline::picture_at(double seconds) const
{
    const TimedSubtitle *found = nullptr;
    for (std::size_t i = 0; i < items_.size(); ++i)
        if (!items_[i].bitmap.rgba.empty() && showing(i, seconds))
            found = &items_[i];
    return found;
}

} // namespace hui::media
