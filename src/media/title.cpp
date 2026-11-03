// Mediathek - Turns a movie file name into a display title, year and quality.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/title.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <vector>

namespace hui::media
{

namespace
{

constexpr std::array<std::string_view, 12> kVideoExtensions = {
    "mkv", "mp4", "m4v", "avi", "mov", "ts", "m2ts", "webm", "wmv", "mpg", "mpeg", "mts",
};
constexpr std::array<std::string_view, 3> kImageExtensions = {"jpg", "jpeg", "png"};
constexpr std::array<std::string_view, 2> kSubtitleExtensions = {"srt", "vtt"};

// Words that start the release-tag tail of a scene name. Everything after the
// first of them is not part of the title.
constexpr std::array<std::string_view, 40> kTagWords = {
    "2160p",  "1080p",   "1080i", "720p",   "576p",   "480p",   "4k",       "uhd",
    "bluray", "blu-ray", "bdrip", "brrip",  "webrip", "web-dl", "webdl",    "hdtv",
    "dvdrip", "dvd",     "hdrip", "remux",  "x264",   "x265",   "h264",     "h265",
    "hevc",   "avc",     "xvid",  "divx",   "hdr",    "hdr10",  "dv",       "dts",
    "ac3",    "aac",     "atmos", "truehd", "proper", "repack", "extended", "imax",
};

std::string_view extension_of(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size() || name.size() - dot > 6)
        return {};
    return name.substr(dot + 1);
}

template <std::size_t N>
bool has_extension(std::string_view name, const std::array<std::string_view, N> &list)
{
    const std::string extension = lower(extension_of(name));
    if (extension.empty())
        return false;
    return std::find(list.begin(), list.end(), std::string_view(extension)) != list.end();
}

bool is_year(std::string_view word)
{
    if (word.size() != 4)
        return false;
    for (const char c : word)
        if (c < '0' || c > '9')
            return false;
    const int year =
        (word[0] - '0') * 1000 + (word[1] - '0') * 100 + (word[2] - '0') * 10 + (word[3] - '0');
    return year >= 1900 && year <= 2100;
}

int to_year(std::string_view word)
{
    return (word[0] - '0') * 1000 + (word[1] - '0') * 100 + (word[2] - '0') * 10 + (word[3] - '0');
}

bool is_tag_word(const std::string &word)
{
    return std::find(kTagWords.begin(), kTagWords.end(), std::string_view(word)) != kTagWords.end();
}

std::string quality_of(const std::string &word)
{
    if (word == "2160p" || word == "4k" || word == "uhd")
        return "4K";
    if (word == "1080p" || word == "1080i")
        return "1080p";
    if (word == "720p")
        return "720p";
    if (word == "576p" || word == "480p")
        return "SD";
    return {};
}

} // namespace

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_video_file(std::string_view file_name)
{
    return has_extension(file_name, kVideoExtensions);
}

bool is_image_file(std::string_view file_name)
{
    return has_extension(file_name, kImageExtensions);
}

bool is_subtitle_file(std::string_view file_name)
{
    return has_extension(file_name, kSubtitleExtensions);
}

std::string file_name_of(std::string_view path)
{
    const std::size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string directory_of(std::string_view path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string base_name(std::string_view path)
{
    std::string name = file_name_of(path);
    const std::string_view extension = extension_of(name);
    if (!extension.empty())
        name.resize(name.size() - extension.size() - 1);
    return name;
}

ParsedName parse_file_name(std::string_view file_name)
{
    ParsedName parsed;
    const std::string stem = base_name(file_name);

    // Split into words on dots, underscores, spaces and brackets. A dot inside
    // "5.1" or "Mr. X" costs nothing: both survive as separate words.
    std::vector<std::string> words;
    std::string word;
    const auto flush = [&]()
    {
        if (!word.empty())
            words.push_back(word);
        word.clear();
    };
    for (const char c : stem)
    {
        if (c == '.' || c == '_' || c == ' ' || c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '{' || c == '}')
            flush();
        else
            word.push_back(c);
    }
    flush();

    // The release tags begin at the first tag word or the year; the year is
    // taken from the last bare year before a tag (titles can contain years:
    // "2001 A Space Odyssey 1968").
    std::size_t end = words.size();
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        const std::string key = lower(words[i]);
        if (is_tag_word(key))
        {
            end = std::min(end, i);
            const std::string q = quality_of(key);
            if (!q.empty() && parsed.quality.empty())
                parsed.quality = q;
        }
    }
    for (std::size_t i = 0; i < end; ++i)
    {
        if (!is_year(words[i]))
            continue;
        // A year that is the first word is part of the title ("2012", "1917").
        if (i == 0)
            continue;
        parsed.year = to_year(words[i]);
        end = i;
    }
    // Tags after the year still carry the quality.
    for (std::size_t i = end; i < words.size(); ++i)
    {
        const std::string q = quality_of(lower(words[i]));
        if (!q.empty() && parsed.quality.empty())
            parsed.quality = q;
    }
    // "1917.2019.mkv": only a year, and it is the title. Take the first word.
    if (end == 0 && !words.empty())
        end = 1;
    parsed.tail.assign(words.begin() + static_cast<std::ptrdiff_t>(end), words.end());

    for (std::size_t i = 0; i < end; ++i)
    {
        if (!parsed.title.empty())
            parsed.title.push_back(' ');
        parsed.title += words[i];
    }
    // Scene names are all-lowercase or dotted; make the title read well.
    const bool shouting = std::none_of(parsed.title.begin(), parsed.title.end(), [](char c)
                                       { return std::islower(static_cast<unsigned char>(c)); });
    const bool whispering = std::none_of(parsed.title.begin(), parsed.title.end(), [](char c)
                                         { return std::isupper(static_cast<unsigned char>(c)); });
    if (shouting || whispering)
    {
        bool start = true;
        for (char &c : parsed.title)
        {
            c = start ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                      : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            start = c == ' ';
        }
    }
    if (parsed.title.empty())
        parsed.title = stem;
    return parsed;
}

namespace
{

bool all_digits(std::string_view text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c)
                                        { return std::isdigit(static_cast<unsigned char>(c)); });
}

int to_number(std::string_view digits)
{
    int value = 0;
    for (const char c : digits.substr(0, 4))
        value = value * 10 + (c - '0');
    return value;
}

// "S01E02", "s1e2", "S01E01E02", "S01E01-E02" (the first episode counts): season and episode.
bool season_episode_word(const std::string &word, int *season, int *episode)
{
    const std::string key = lower(word);
    if (key.size() < 4 || key[0] != 's')
        return false;
    std::size_t at = 1;
    while (at < key.size() && std::isdigit(static_cast<unsigned char>(key[at])))
        ++at;
    if (at == 1 || at > 3 || at >= key.size() || key[at] != 'e')
        return false;
    const std::size_t start = at + 1;
    std::size_t end = start;
    while (end < key.size() && std::isdigit(static_cast<unsigned char>(key[end])))
        ++end;
    if (end == start || end - start > 3)
        return false;
    // Only more episode marks may follow: "E02", "-E02", "-02".
    for (std::size_t i = end; i < key.size(); ++i)
        if (key[i] != 'e' && key[i] != '-' && !std::isdigit(static_cast<unsigned char>(key[i])))
            return false;
    *season = to_number(std::string_view(key).substr(1, at - 1));
    *episode = to_number(std::string_view(key).substr(start, end - start));
    return true;
}

// "1x02", "01x002": season and episode. A picture size ("1920x1080") is too long for it.
bool cross_word(const std::string &word, int *season, int *episode)
{
    const std::size_t x = lower(word).find('x');
    if (x == std::string::npos || x == 0 || x > 2 || word.size() - x - 1 < 2 ||
        word.size() - x - 1 > 3)
        return false;
    const std::string_view left = std::string_view(word).substr(0, x);
    const std::string_view right = std::string_view(word).substr(x + 1);
    if (!all_digits(left) || !all_digits(right))
        return false;
    *season = to_number(left);
    *episode = to_number(right);
    return true;
}

// "S01" alone (an "E02" follows as its own word), or "E02" alone: the number, else -1.
int prefixed_number(const std::string &word, char prefix)
{
    const std::string key = lower(word);
    if (key.size() < 2 || key.size() > 4 || key[0] != prefix ||
        !all_digits(std::string_view(key).substr(1)))
        return -1;
    return to_number(std::string_view(key).substr(1));
}

// The words of a name: split at dots, underscores, blanks and brackets; a word of dashes alone
// ("Show - 1x02 - Title") is a separator too.
std::vector<std::string> name_words(std::string_view stem)
{
    std::vector<std::string> words;
    std::string word;
    const auto flush = [&]()
    {
        if (!word.empty() && word.find_first_not_of('-') != std::string::npos)
            words.push_back(word);
        word.clear();
    };
    for (const char c : stem)
    {
        if (c == '.' || c == '_' || c == ' ' || c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '{' || c == '}')
            flush();
        else
            word.push_back(c);
    }
    flush();
    return words;
}

// Some words as a title, the way a film's name is read (release tags and the year go).
std::string words_title(const std::vector<std::string> &words, std::size_t begin, std::size_t end)
{
    // Nothing but release tags ("S01E02.1080p.mkv") is no title; parse_file_name would make
    // the first tag one.
    if (begin >= end || begin >= words.size() || is_tag_word(lower(words[begin])))
        return {};
    std::string joined;
    for (std::size_t i = begin; i < end && i < words.size(); ++i)
        joined += (joined.empty() ? "" : " ") + words[i];
    return parse_file_name(joined + ".mkv").title;
}

} // namespace

EpisodeName parse_episode_name(std::string_view file_name)
{
    EpisodeName name;
    const std::vector<std::string> words = name_words(base_name(file_name));
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        int season = -1;
        int episode = 0;
        std::size_t after = i + 1;
        if (!season_episode_word(words[i], &season, &episode) &&
            !cross_word(words[i], &season, &episode))
        {
            // "S01 E02" in two words.
            const int alone = prefixed_number(words[i], 's');
            const int next = i + 1 < words.size() ? prefixed_number(words[i + 1], 'e') : -1;
            if (alone < 0 || next < 0)
                continue;
            season = alone;
            episode = next;
            after = i + 2;
        }
        name.marked = true;
        name.season = season;
        name.episode = episode;
        name.show = words_title(words, 0, i);
        name.title = words_title(words, after, words.size());
        return name;
    }
    // No mark: a number on its own still counts the episode.
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        const std::string key = lower(words[i]);
        int number = prefixed_number(words[i], 'e');
        std::size_t after = i + 1;
        if (number < 0 && (key == "episode" || key == "folge" || key == "ep") &&
            i + 1 < words.size() && all_digits(words[i + 1]) && words[i + 1].size() <= 3)
        {
            number = to_number(words[i + 1]);
            after = i + 2;
        }
        if (number < 0 && i == 0 && all_digits(key) && key.size() <= 3)
            number = to_number(key);
        if (number < 0)
            continue;
        name.episode = number;
        name.show = words_title(words, 0, i);
        name.title = words_title(words, after, words.size());
        return name;
    }
    name.title = words_title(words, 0, words.size());
    return name;
}

int season_of_folder(std::string_view folder_name)
{
    const std::string key = lower(folder_name);
    if (key == "specials" || key == "special" || key == "extras")
        return 0;
    for (const std::string_view word : {"season", "staffel", "saison", "series", "s"})
    {
        if (key.size() <= word.size() || key.compare(0, word.size(), word) != 0)
            continue;
        std::string_view rest = std::string_view(key).substr(word.size());
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '.' ||
                                 rest.front() == '_' || rest.front() == '-'))
            rest.remove_prefix(1);
        if (all_digits(rest) && rest.size() <= 3)
            return to_number(rest);
    }
    return -1;
}

std::string sort_key(std::string_view title)
{
    std::string key;
    for (const char c : title)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ')
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (const char *article : {"the ", "a ", "an "})
    {
        const std::string_view prefix(article);
        if (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0)
        {
            key.erase(0, prefix.size());
            break;
        }
    }
    return key;
}

std::string search_fold(std::string_view text)
{
    // U+00C0 to U+00FF, one letter each; the signs (x, /) and the letters without a plain base
    // (the ligature, thorn, eth) stand for themselves or their nearest.
    static constexpr std::string_view kLatin1 = // 64 entries, the sharp s is handled apart

        "aaaaaaaceeeeiiiidnooooo ouuuuytsaaaaaaaceeeeiiiidnooooo ouuuuyty";
    std::string out;
    bool gap = true; // no leading space
    const auto word = [&](char c)
    {
        out.push_back(c);
        gap = false;
    };
    const auto space = [&]
    {
        if (!gap)
            out.push_back(' ');
        gap = true;
    };
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0xC3 && i + 1 < text.size() &&
            (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80)
        {
            const std::size_t at = static_cast<unsigned char>(text[++i]) - 0x80;
            if (at == 0x1F) // the sharp s
            {
                word('s');
                word('s');
            }
            else if (kLatin1[at] == ' ')
                space();
            else
                word(kLatin1[at]);
        }
        else if (c >= 0x80)
            word(static_cast<char>(c));
        else if (std::isalnum(c))
            word(static_cast<char>(std::tolower(c)));
        else
            space();
    }
    if (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

} // namespace hui::media
