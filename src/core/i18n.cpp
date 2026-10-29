// Mediathek - The texts the app shows, in English and German.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/i18n.hpp"

#include <atomic>
#include <cctype>

namespace hui::i18n
{

namespace
{

struct Entry
{
    const char *english;
    const char *german;
};

constexpr Entry kTexts[] = {
#define HUI_TEXT(id, english, german) {english, german},
#include "core/i18n_texts.inc"
#undef HUI_TEXT
};

static_assert(sizeof(kTexts) / sizeof(kTexts[0]) == static_cast<std::size_t>(Text::count_),
              "every Text has a table row");

// Written once at start-up; the cover and scan threads read it afterwards.
std::atomic<Language> g_language{Language::english};

// SCE_SYSTEM_PARAM_LANG_GERMAN
constexpr int kConsoleGerman = 4;

std::string lowered(std::string_view text)
{
    std::string out;
    for (const char c : text)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

} // namespace

Language language()
{
    return g_language.load(std::memory_order_relaxed);
}

void set_language(Language language)
{
    g_language.store(language, std::memory_order_relaxed);
}

Language from_console_language(int code)
{
    return code == kConsoleGerman ? Language::german : Language::english;
}

bool parse_language(std::string_view name, Language *out)
{
    const std::string value = lowered(name);
    // "de", "de-DE", "de_AT", "german", "deutsch"; "en", "en-GB", "english".
    const std::string_view head = std::string_view(value).substr(0, 2);
    if (value == "german" || value == "deutsch" ||
        (head == "de" && (value.size() == 2 || value[2] == '-' || value[2] == '_')))
    {
        *out = Language::german;
        return true;
    }
    if (value == "english" || value == "englisch" ||
        (head == "en" && (value.size() == 2 || value[2] == '-' || value[2] == '_')))
    {
        *out = Language::english;
        return true;
    }
    return false;
}

const char *tmdb_language(Language language)
{
    return language == Language::german ? "de-DE" : "en-US";
}

const char *tr(Text text)
{
    const auto index = static_cast<std::size_t>(text);
    if (index >= static_cast<std::size_t>(Text::count_))
        return "";
    const Entry &entry = kTexts[index];
    return language() == Language::german ? entry.german : entry.english;
}

std::string format(Text text, std::string_view value)
{
    std::string line = tr(text);
    const std::size_t at = line.find("{}");
    if (at != std::string::npos)
        line.replace(at, 2, value);
    return line;
}

std::string movie_count(std::size_t count)
{
    return count == 1 ? tr(Text::movies_one) : format(Text::movies_many, std::to_string(count));
}

} // namespace hui::i18n
