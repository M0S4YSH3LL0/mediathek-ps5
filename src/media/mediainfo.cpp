// Mediathek - What a movie file contains: length, codecs, audio and subtitle languages.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/mediainfo.hpp"

#include "core/i18n.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hui::media
{

namespace
{
using i18n::Text;

struct Language
{
    const char *canonical;   // ISO 639-2/B
    const char *terminology; // ISO 639-2/T when it differs, else ""
    const char *alpha2;
    const char *word;   // what a file name spells out ("english"), or ""
    const char *native; // the language's own name for itself ("deutsch"), or ""
    Text name;
};

constexpr Language kLanguages[] = {
    {"eng", "", "en", "english", "", Text::lang_eng},
    {"ger", "deu", "de", "german", "deutsch", Text::lang_ger},
    {"fre", "fra", "fr", "french", "francais", Text::lang_fre},
    {"spa", "", "es", "spanish", "espanol", Text::lang_spa},
    {"ita", "", "it", "italian", "italiano", Text::lang_ita},
    {"por", "", "pt", "portuguese", "portugues", Text::lang_por},
    {"dut", "nld", "nl", "dutch", "nederlands", Text::lang_dut},
    {"rus", "", "ru", "russian", "", Text::lang_rus},
    {"pol", "", "pl", "polish", "polski", Text::lang_pol},
    {"tur", "", "tr", "turkish", "turkce", Text::lang_tur},
    {"jpn", "", "ja", "japanese", "", Text::lang_jpn},
    {"kor", "", "ko", "korean", "", Text::lang_kor},
    {"chi", "zho", "zh", "chinese", "", Text::lang_chi},
    {"ara", "", "ar", "arabic", "", Text::lang_ara},
    {"hin", "", "hi", "hindi", "", Text::lang_hin},
    {"swe", "", "sv", "swedish", "svenska", Text::lang_swe},
    {"nor", "nob", "no", "norwegian", "norsk", Text::lang_nor},
    {"dan", "", "da", "danish", "dansk", Text::lang_dan},
    {"fin", "", "fi", "finnish", "suomi", Text::lang_fin},
    {"cze", "ces", "cs", "czech", "cesky", Text::lang_cze},
    {"hun", "", "hu", "hungarian", "magyar", Text::lang_hun},
    {"gre", "ell", "el", "greek", "", Text::lang_gre},
    {"heb", "", "he", "hebrew", "", Text::lang_heb},
    {"ukr", "", "uk", "ukrainian", "", Text::lang_ukr},
    {"rum", "ron", "ro", "romanian", "", Text::lang_rum},
    {"tha", "", "th", "thai", "", Text::lang_tha},
    {"vie", "", "vi", "vietnamese", "", Text::lang_vie},
    {"ind", "", "id", "indonesian", "", Text::lang_ind},
};

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

const Language *find_language(const std::string &word)
{
    if (word.empty())
        return nullptr;
    for (const Language &language : kLanguages)
        if (word == language.canonical || word == language.terminology || word == language.alpha2 ||
            word == language.word || word == language.native)
            return &language;
    return nullptr;
}

// A code is letters only and short; the file's own tags are not trusted.
bool plausible_code(const std::string &code)
{
    return code.size() >= 2 && code.size() <= 3 &&
           std::all_of(code.begin(), code.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

} // namespace

std::string canonical_language(std::string_view code)
{
    const std::string word = lower(code);
    if (const Language *known = find_language(word); known != nullptr)
        return known->canonical;
    if (word == "und" || word == "mis" || word == "mul" || word == "zxx" || !plausible_code(word))
        return "und";
    return word;
}

std::string language_name(std::string_view canonical)
{
    const std::string word = lower(canonical);
    if (const Language *known = find_language(word); known != nullptr)
        return i18n::tr(known->name);
    if (word.empty() || word == "und")
        return i18n::tr(Text::lang_und);
    std::string upper = word;
    for (char &c : upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return upper;
}

std::string language_from_filename(std::string_view file_name)
{
    std::string name = lower(file_name);
    if (const std::size_t dot = name.rfind('.'); dot != std::string::npos)
        name.resize(dot); // the extension
    std::vector<std::string> words;
    std::string current;
    for (const char c : name)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        {
            current += c;
            continue;
        }
        if (!current.empty())
            words.push_back(std::move(current));
        current.clear();
    }
    if (!current.empty())
        words.push_back(std::move(current));
    // "It (2017).srt" is a film called It, not Italian: a language needs a name before it.
    for (std::size_t at = words.size(); at > 1 && words.size() - at < 3; --at)
        if (const Language *known = find_language(words[at - 1]); known != nullptr)
            return known->canonical;
    return std::string();
}

std::string genre_name(int tmdb_id)
{
    struct Genre
    {
        int id;
        Text name;
    };
    // TMDB's numbers for movies; they have not changed in years.
    static constexpr Genre kGenres[] = {
        {28, Text::genre_action},      {12, Text::genre_adventure},  {16, Text::genre_animation},
        {35, Text::genre_comedy},      {80, Text::genre_crime},      {99, Text::genre_documentary},
        {18, Text::genre_drama},       {10751, Text::genre_family},  {14, Text::genre_fantasy},
        {36, Text::genre_history},     {27, Text::genre_horror},     {10402, Text::genre_music},
        {9648, Text::genre_mystery},   {10749, Text::genre_romance}, {878, Text::genre_scifi},
        {10770, Text::genre_tv_movie}, {53, Text::genre_thriller},   {10752, Text::genre_war},
        {37, Text::genre_western},
    };
    for (const Genre &genre : kGenres)
        if (genre.id == tmdb_id)
            return i18n::tr(genre.name);
    return std::string();
}

std::string format_runtime(double seconds)
{
    if (seconds < 30.0)
        return std::string();
    const int minutes = static_cast<int>((seconds + 30.0) / 60.0); // rounded
    char text[32];
    if (minutes >= 60)
        std::snprintf(text, sizeof(text), "%dh %02dm", minutes / 60, minutes % 60);
    else
        std::snprintf(text, sizeof(text), "%dm", minutes);
    return text;
}

std::string resolution_label(int width, int height)
{
    if (height <= 0 && width <= 0)
        return std::string();
    // Widescreen films keep their width and lose picture height to black bars.
    if (width >= 3200 || height >= 2000)
        return "4K";
    if (width >= 1800 || height >= 1000)
        return "1080p";
    if (width >= 1200 || height >= 700)
        return "720p";
    return "SD";
}

void add_language(std::vector<std::string> *list, std::string_view canonical)
{
    if (std::find(list->begin(), list->end(), canonical) == list->end())
        list->emplace_back(canonical);
}

std::vector<std::string> language_names(const std::vector<std::string> &canonical)
{
    std::vector<std::string> names;
    for (const std::string &code : canonical)
        if (code != "und")
            names.push_back(language_name(code));
    if (names.empty() && !canonical.empty())
        names.push_back(language_name("und"));
    return names;
}

namespace
{

// Version 2 offers only audio tracks the app can decode; a version 1 file may list more and
// is read again.
constexpr const char *kHeader = "mediainfo 2";

// One value per line; a value never holds a line break.
std::string one_line(std::string text)
{
    for (char &c : text)
        if (c == '\n' || c == '\r')
            c = ' ';
    return text;
}

std::string join(const std::vector<std::string> &items)
{
    std::string out;
    for (const std::string &item : items)
    {
        if (!out.empty())
            out += ',';
        out += one_line(item);
    }
    return out;
}

std::vector<std::string> split(const std::string &text)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    while (from < text.size())
    {
        std::size_t comma = text.find(',', from);
        if (comma == std::string::npos)
            comma = text.size();
        if (comma > from)
            out.push_back(text.substr(from, comma - from));
        from = comma + 1;
    }
    return out;
}

} // namespace

std::string encode_info(const MediaInfo &info)
{
    char number[32];
    std::string out = std::string(kHeader) + "\n";
    std::snprintf(number, sizeof(number), "%.1f", info.duration);
    out += std::string("duration=") + number + "\n";
    out += "video=" + one_line(info.video_codec) + "\n";
    std::snprintf(number, sizeof(number), "%dx%d", info.width, info.height);
    out += std::string("size=") + number + "\n";
    out += "container=" + one_line(info.container) + "\n";
    out += "audio_format=" + one_line(info.audio_format) + "\n";
    out += "audio=" + join(info.audio_languages) + "\n";
    out += "end\n";
    return out;
}

bool decode_info(std::string_view text, MediaInfo *out)
{
    MediaInfo info;
    bool header = false;
    bool ended = false;
    std::size_t from = 0;
    while (from < text.size() && !ended)
    {
        std::size_t line_end = text.find('\n', from);
        if (line_end == std::string_view::npos)
            line_end = text.size();
        const std::string line(text.substr(from, line_end - from));
        from = line_end + 1;
        if (!header)
        {
            if (line != kHeader)
                return false;
            header = true;
            continue;
        }
        if (line == "end")
        {
            ended = true;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
            return false;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "duration")
            info.duration = std::max(0.0, std::atof(value.c_str()));
        else if (key == "video")
            info.video_codec = value;
        else if (key == "size")
        {
            int width = 0;
            int height = 0;
            if (std::sscanf(value.c_str(), "%dx%d", &width, &height) == 2)
            {
                info.width = std::max(0, width);
                info.height = std::max(0, height);
            }
        }
        else if (key == "container")
            info.container = value;
        else if (key == "audio_format")
            info.audio_format = value;
        else if (key == "audio")
            info.audio_languages = split(value);
    }
    if (!header || !ended)
        return false; // half written
    *out = std::move(info);
    return true;
}

} // namespace hui::media
