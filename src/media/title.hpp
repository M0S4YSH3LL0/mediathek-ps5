// Mediathek - Turns a movie file name into a display title, year and quality.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hui::media
{

struct ParsedName
{
    std::string title;   // "The Long Walk Home"
    int year = 0;        // 0 when the name carries none
    std::string quality; // "4K", "1080p", "720p" or ""
    // The words after the title, as the name has them: {"German", "DL", "1080p", "x264-GRP"}.
    std::vector<std::string> tail;
};

// Understands the usual naming of movie collections:
//   "The Long Walk Home (2014).mkv"
//   "The.Long.Walk.Home.2014.1080p.BluRay.x264-GRP.mkv"
//   "the_long_walk_home [2014] [4K].mp4"
// The extension, separators, the year and release tags (resolution, source,
// codec, group) are dropped from the title.
ParsedName parse_file_name(std::string_view file_name);

// What the file name of a series episode says:
//   "Show.Name.S01E02.The.Pilot.1080p.mkv"  season 1, episode 2, "The Pilot", show "Show Name"
//   "Show Name - 1x02 - The Pilot.mkv"      the same
//   "Show Name S01 E02.mkv"                 the same, no title
// `marked` says the name carries such a mark, which is what makes a file an episode outside a
// series folder. Without one, `season` is -1 and `episode` is what a bare number says ("E05",
// "Episode 5", "Folge 5", "05 - Name"), else 0.
struct EpisodeName
{
    bool marked = false;
    int season = -1;
    int episode = 0;
    std::string show;  // the words before the mark, as a title: "Show Name", or ""
    std::string title; // the words after it, release tags dropped, or ""
};
EpisodeName parse_episode_name(std::string_view file_name);

// The season a folder's name stands for: "Season 1", "Season1", "season 01", "Staffel 2", "S02"
// and "Specials" (0); -1 for any other name.
int season_of_folder(std::string_view folder_name);

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

// What a search compares: lower case, accents and umlauts dropped ("Amélie" -> "amelie",
// "Über" -> "uber", "straße" -> "strasse"), anything that is not a letter or digit a single
// space ("Spider-Man: Home" -> "spider man home"). UTF-8 outside Latin-1 passes through.
std::string search_fold(std::string_view text);

} // namespace hui::media
