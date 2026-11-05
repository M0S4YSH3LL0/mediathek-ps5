// Mediathek - What a movie file contains: length, codecs, audio and subtitle languages.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/mediainfo.hpp"

#include "media/title.hpp"

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
    // A short name that starts with the language ("English.srt", "Deutsch.forced.srt", "ger.srt")
    // is a subtitle file from a "Subs" folder. Spelled out only: "it.srt" stays a film called It.
    if (!words.empty() && words.size() <= 3 && words.front().size() > 2)
        if (const Language *known = find_language(words.front()); known != nullptr)
            return known->canonical;
    return std::string();
}

namespace
{

// The words that tell the languages apart, the ones a subtitle says in every other line, each
// list as " word word " so a lookup is one find. Words two languages share ("die", "was", "que",
// "una", "para") are left out.
struct Telltales
{
    const char *canonical;
    std::string_view words;
};

constexpr Telltales kTelltales[] = {
    {"eng", " the and you that with have this what are not your but they will just from there "
            "would know going "},
    {"ger", " und der das nicht ich ist ein eine du sie mit auf wir auch sich dass noch wie bin "
            "habe "},
    {"fre", " les des est pas vous une nous je pour dans qui avec sont mais tout oui suis ici "
            "elle tu "},
    {"spa", " los las como usted muy bien yo nada tengo todo pero esto esta estoy eres quiero "
            "porque hay "},
    {"ita", " che non sono per della questo ho cosa gli anche tutto niente ciao essere voglio "
            "perche mio lei siamo "},
    {"por", " voce uma com mais isso ele eu tenho obrigado nao estou muito tudo ela sim vamos ser "
            "foi "},
    {"dut", " het een niet ik je dat van wat maar zijn ook hebt jij geen mijn hij wij naar gewoon "
            "dus "},
};

} // namespace

std::string language_from_text(std::string_view text)
{
    constexpr std::size_t kLanguageCount = sizeof(kTelltales) / sizeof(kTelltales[0]);
    constexpr std::size_t kSample = 60000; // the first minutes say it all
    int hits[kLanguageCount] = {};
    std::string word;
    const std::size_t end = std::min(text.size(), kSample);
    for (std::size_t i = 0; i <= end; ++i)
    {
        const unsigned char c = i < end ? static_cast<unsigned char>(text[i]) : ' ';
        if (c < 0x80 && std::isalpha(c) != 0)
        {
            word.push_back(static_cast<char>(std::tolower(c)));
            continue;
        }
        if (word.size() >= 2 && word.size() <= 8)
        {
            const std::string padded = " " + word + " ";
            for (std::size_t language = 0; language < kLanguageCount; ++language)
                if (kTelltales[language].words.find(padded) != std::string_view::npos)
                    ++hits[language];
        }
        word.clear();
    }
    std::size_t best = 0;
    for (std::size_t language = 1; language < kLanguageCount; ++language)
        if (hits[language] > hits[best])
            best = language;
    int runner_up = 0;
    for (std::size_t language = 0; language < kLanguageCount; ++language)
        if (language != best && hits[language] > runner_up)
            runner_up = hits[language];
    // Sure only with plenty of words and a clear lead: a wrong name is worse than none.
    return hits[best] >= 20 && hits[best] >= 2 * runner_up ? kTelltales[best].canonical
                                                           : std::string();
}

namespace
{

// A language a film's name may say outright: its English or own name, or the codes release
// names use. The two-letter codes are left out: "it", "no" and "de" are words in titles.
const Language *named_language(const std::string &piece)
{
    const Language *known = find_language(piece);
    if (known == nullptr)
        return nullptr;
    const bool spelled = piece == known->word || piece == known->native;
    return spelled || piece == "ger" || piece == "deu" || piece == "eng" ? known : nullptr;
}

// Lower-case letters and digits, one piece at a time: "German-GRP" is "german" and "grp".
void add_pieces(std::string_view text, std::vector<std::string> *pieces)
{
    std::string current;
    for (const char c : text)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            continue;
        }
        if (!current.empty())
            pieces->push_back(std::move(current));
        current.clear();
    }
    if (!current.empty())
        pieces->push_back(std::move(current));
}

bool says_subtitles(const std::string &piece)
{
    return piece.compare(0, 3, "sub") == 0 || piece == "untertitel";
}

} // namespace

std::vector<std::string> audio_languages_from_name(std::string_view file_name)
{
    const ParsedName parsed = parse_file_name(file_name);
    std::vector<std::string> pieces;
    if (parsed.year == 0)
    {
        // "Film German 1080p.mkv": with no year nothing marks where the title ends, so a
        // language right at its end ("Film German", "Film German DL") is taken as well.
        std::vector<std::string> title;
        add_pieces(parsed.title, &title);
        std::size_t from = title.size();
        while (from > 1 && (named_language(title[from - 1]) != nullptr || title[from - 1] == "dl"))
            --from;
        pieces.assign(title.begin() + static_cast<std::ptrdiff_t>(from), title.end());
    }
    for (const std::string &word : parsed.tail)
        add_pieces(word, &pieces);

    std::vector<std::string> found;
    for (std::size_t i = 0; i < pieces.size(); ++i)
    {
        const Language *known = named_language(pieces[i]);
        if (known == nullptr)
            continue;
        if ((i > 0 && says_subtitles(pieces[i - 1])) ||
            (i + 1 < pieces.size() && says_subtitles(pieces[i + 1])))
            continue;
        add_language(&found, known->canonical);
    }
    return found;
}

std::vector<std::string> audio_languages_to_show(const MediaInfo *info, bool read,
                                                 std::string_view file_name)
{
    std::vector<std::string> listed;
    if (info != nullptr)
        listed = info->audio_languages;
    const bool tagged = std::any_of(listed.begin(), listed.end(),
                                    [](const std::string &code) { return code != "und"; });
    if (tagged || !read)
        return listed;
    std::vector<std::string> named = audio_languages_from_name(file_name);
    return named.empty() ? listed : named;
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
        {37, Text::genre_western},     {10763, Text::genre_news},    {10764, Text::genre_reality},
        {10766, Text::genre_soap},     {10767, Text::genre_talk},
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
// is read again. Version 3 adds the subtitle tracks inside the file, which version 2 did not
// look at: its files are read again too.
constexpr const char *kHeader = "mediainfo 3";

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
    out += "subtitles=" + join(info.subtitle_languages) + "\n";
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
        else if (key == "subtitles")
            info.subtitle_languages = split(value);
    }
    if (!header || !ended)
        return false; // half written
    *out = std::move(info);
    return true;
}

} // namespace hui::media
