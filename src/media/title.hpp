// Mediathek - Turns a movie file name into a display title, year and quality.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace hui::media
{

struct ParsedName
{
    std::string title;   // "The Long Walk Home"
    int year = 0;        // 0 when the name carries none
    std::string quality; // "4K", "1080p", "720p" or ""
};

// Understands the usual naming of movie collections:
//   "The Long Walk Home (2014).mkv"
//   "The.Long.Walk.Home.2014.1080p.BluRay.x264-GRP.mkv"
//   "the_long_walk_home [2014] [4K].mp4"
// The extension, separators, the year and release tags (resolution, source,
// codec, group) are dropped from the title.
ParsedName parse_file_name(std::string_view file_name);

// True for the containers the player can open.
bool is_video_file(std::string_view file_name);
bool is_image_file(std::string_view file_name);
bool is_subtitle_file(std::string_view file_name);

// Lowercase ASCII copy; names are compared case-insensitively.
std::string lower(std::string_view text);

// The file name without its directory, and without its extension.
std::string base_name(std::string_view path);
std::string directory_of(std::string_view path);
std::string file_name_of(std::string_view path);

// A sort key that ignores case, leading articles and punctuation, so that
// "The Matrix" sits among the Ms.
std::string sort_key(std::string_view title);

} // namespace hui::media
