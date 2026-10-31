// Mediathek - The texts the app shows, in English and German.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace hui::i18n
{

enum class Language : unsigned char
{
    english, // the default, and what any language the app lacks falls back to
    german,
};

// Every fixed text of the app. Each one has both translations in the table in
// i18n.cpp, so a text cannot be added without its German.
enum class Text : unsigned short
{
#define HUI_TEXT(id, english, german) id,
#include "core/i18n_texts.inc"
#undef HUI_TEXT
    count_,
};

// The language the app speaks. Set once at start-up, before other threads run.
Language language();
void set_language(Language language);

// The console's own language setting (SCE_SYSTEM_PARAM_LANG_*): German when it
// says German, English for everything else, including a value that is not one.
Language from_console_language(int code);

// The language a Settings::ui_language choice means: 1 English, 2 German, anything else
// (0, "automatic") the language the console or UI_LANGUAGE gave, `automatic`.
Language choose_language(int setting, Language automatic);

// "en", "en-US", "english", "de", "de-DE", "deutsch" ...; false for anything
// else (including "auto", which means: ask the console).
bool parse_language(std::string_view name, Language *out);

// The matching TMDB language code, "en-US" or "de-DE".
const char *tmdb_language(Language language);

// The text in the current language. Never null, never empty.
const char *tr(Text text);

// tr() with its first "{}" replaced by `value` (a time, a name, a number).
std::string format(Text text, std::string_view value);

// "1 Film", "26 Filme": the count line of the page header.
std::string movie_count(std::size_t count);

} // namespace hui::i18n
