// Mediathek - SubRip (.srt) subtitles: parsing and lookup by time.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/subtitles.hpp"

#include <algorithm>
#include <cstdio>

namespace hui::media
{

namespace
{

// "00:01:02,500" or "00:01:02.500" or "01:02,500".
bool parse_time(std::string_view text, double *seconds)
{
    const std::string copy(text);
    int h = 0;
    int m = 0;
    int s = 0;
    int ms = 0;
    if (std::sscanf(copy.c_str(), "%d:%d:%d%*[,.]%d", &h, &m, &s, &ms) == 4 ||
        (h = 0, std::sscanf(copy.c_str(), "%d:%d%*[,.]%d", &m, &s, &ms) == 3))
    {
        *seconds = h * 3600.0 + m * 60.0 + s + ms / 1000.0;
        return true;
    }
    return false;
}

// Removes <i>, <b>, <font ...> and {\an8} style tags.
std::string strip_markup(std::string_view line)
{
    std::string out;
    int depth_angle = 0;
    int depth_brace = 0;
    for (const char c : line)
    {
        if (c == '<')
            ++depth_angle;
        else if (c == '>' && depth_angle > 0)
            --depth_angle;
        else if (c == '{')
            ++depth_brace;
        else if (c == '}' && depth_brace > 0)
            --depth_brace;
        else if (depth_angle == 0 && depth_brace == 0 && c != '\r')
            out.push_back(c);
    }
    return out;
}

} // namespace

bool Subtitles::parse(std::string_view text)
{
    cues_.clear();
    last_ = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf)
        text.remove_prefix(3);

    std::size_t start = 0;
    Cue cue;
    bool in_cue = false;
    const auto finish = [&]()
    {
        if (in_cue && !cue.text.empty() && cue.end > cue.start)
            cues_.push_back(cue);
        cue = Cue{};
        in_cue = false;
    };
    while (start <= text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        const std::size_t arrow = line.find("-->");
        if (arrow != std::string_view::npos)
        {
            finish();
            const auto trim = [](std::string_view v)
            {
                while (!v.empty() && v.front() == ' ')
                    v.remove_prefix(1);
                while (!v.empty() && v.back() == ' ')
                    v.remove_suffix(1);
                return v;
            };
            std::string_view right = trim(line.substr(arrow + 3));
            const std::size_t space = right.find(' ');
            if (space != std::string_view::npos)
                right = right.substr(0, space); // position hints follow the end time
            if (parse_time(trim(line.substr(0, arrow)), &cue.start) && parse_time(right, &cue.end))
                in_cue = true;
            continue;
        }
        if (line.empty())
        {
            finish();
            continue;
        }
        if (!in_cue)
            continue; // the cue number, or junk
        const std::string clean = strip_markup(line);
        if (clean.empty())
            continue;
        if (!cue.text.empty())
            cue.text.push_back('\n');
        cue.text += clean;
        if (start > text.size())
            break;
    }
    finish();
    std::stable_sort(cues_.begin(), cues_.end(),
                     [](const Cue &a, const Cue &b) { return a.start < b.start; });
    return !cues_.empty();
}

const Cue *Subtitles::at(double seconds) const
{
    if (cues_.empty())
        return nullptr;
    // Playback moves forward almost always: start from the last answer.
    std::size_t i = last_ < cues_.size() && cues_[last_].start <= seconds ? last_ : 0;
    for (; i < cues_.size() && cues_[i].start <= seconds; ++i)
    {
        if (seconds < cues_[i].end)
        {
            last_ = i;
            return &cues_[i];
        }
    }
    // Overlapping cues: look back a little.
    for (std::size_t back = std::min<std::size_t>(i, 4); back > 0; --back)
        if (seconds < cues_[i - back].end && seconds >= cues_[i - back].start)
            return &cues_[i - back];
    return nullptr;
}

} // namespace hui::media
