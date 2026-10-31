// Mediathek - What a movie file contains: length, codecs, audio and subtitle languages.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hui::media
{

// Read from the file's headers by a probe (src/platform/ps5/media_probe.cpp on a
// console); nothing here needs FFmpeg, so the tests can cover all of it.
struct MediaInfo
{
    double duration = 0.0;   // seconds; 0 = unknown
    std::string video_codec; // "H.264", "HEVC", ... or ""
    int width = 0;
    int height = 0;
    std::string container;    // "MKV", "MP4", ... or ""
    std::string audio_format; // the default track: "AC3 5.1", or ""
    // Canonical codes ("eng"), "und" when untagged, of the audio tracks the app can decode:
    // a track it cannot play is not offered.
    std::vector<std::string> audio_languages;
};

// A line-based text for the cache file; decode_info() is its inverse and false
// for anything that is not one (a half-written or foreign file).
std::string encode_info(const MediaInfo &info);
bool decode_info(std::string_view text, MediaInfo *out);

// "eng" for "eng", "en", "English"; "ger" for "deu", "de", "Deutsch" ...: the
// ISO 639-2 code with one spelling per language, "und" when it is empty or not
// a language, and the lower-cased input when it is a code this table lacks.
std::string canonical_language(std::string_view code);

// The name in the app's language ("English", "Deutsch"); the upper-cased code
// for a language the table lacks, a text for "und".
std::string language_name(std::string_view canonical);

// The language a subtitle file's name says: "Film (2011).de.srt" and
// "2_English.srt" are German and English; "" when it says nothing.
std::string language_from_filename(std::string_view file_name);

// The language a subtitle file's text is in, guessed from the words it uses; "" when it has too
// few lines or none of the languages the app knows by their common words stands out. For the
// subtitle files whose name says nothing.
std::string language_from_text(std::string_view subtitle_text);

// The languages a film file's name says its sound is in: "Film (2011) German DL 1080p.mkv" is
// {"ger"}, "Film.2011.German.English.mkv" {"ger", "eng"}. Only what comes after the title counts
// (a film called "The English Patient" is not in English for that), only a language's name or
// "ger"/"deu"/"eng" is taken (a film called "It" or "No" is not Italian or Norwegian), and a
// language the name calls subtitles ("Eng.Sub") is none. Empty when the name says nothing.
std::vector<std::string> audio_languages_from_name(std::string_view file_name);

// The sound languages to list for a film: what the file's headers say; and when they say
// nothing (no tag at all, or "und" on every track) or the file could not be read, what its
// name says, if anything. `read` is false while the file has not been looked at yet, when a
// name guess would only be replaced a moment later. `info` is null when nothing was read.
std::vector<std::string> audio_languages_to_show(const MediaInfo *info, bool read,
                                                 std::string_view file_name);

// The name of a TMDB genre number in the app's language ("Comedy", "Komödie");
// "" for a number the table lacks.
std::string genre_name(int tmdb_id);

// "2h 14m", "47m"; "" for no length.
std::string format_runtime(double seconds);

// "1080p", "4K", "720p", "SD" from the picture's height; "" when unknown.
std::string resolution_label(int width, int height);

// Adds a code to a list unless it is there already.
void add_language(std::vector<std::string> *list, std::string_view canonical);

// The names of a list of codes, in the app's language, in order. "und" (untagged)
// is shown only when nothing else is known.
std::vector<std::string> language_names(const std::vector<std::string> &canonical);

} // namespace hui::media
