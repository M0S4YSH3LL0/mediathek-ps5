// Mediathek - Tests for the texts: both languages, console detection, formatting.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/i18n.hpp"
#include "media/library.hpp"
#include "media/tmdb.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace
{

using namespace hui::i18n;

// Every test leaves the language as it found it: it is a process-wide setting.
class I18nTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        saved_ = language();
    }
    void TearDown() override
    {
        set_language(saved_);
    }

  private:
    Language saved_ = Language::english;
};

TEST_F(I18nTest, EveryTextHasBothLanguages)
{
    std::set<std::string> english;
    int same = 0;
    for (int i = 0; i < static_cast<int>(Text::count_); ++i)
    {
        const auto text = static_cast<Text>(i);
        set_language(Language::english);
        const std::string en = tr(text);
        set_language(Language::german);
        const std::string de = tr(text);
        EXPECT_FALSE(en.empty()) << "text " << i;
        EXPECT_FALSE(de.empty()) << "text " << i;
        // A "{}" is where a value goes: both languages must have one, or none.
        EXPECT_EQ(en.find("{}") != std::string::npos, de.find("{}") != std::string::npos)
            << en << " / " << de;
        same += en == de ? 1 : 0;
    }
    // A few words are the same in both (Details, Pause, STATUS, USB {}), and so are about
    // ten genre and language names (Drama, Horror, Hindi, Science Fiction ...); most are not.
    EXPECT_LT(same, 24);
}

TEST_F(I18nTest, GermanTextsAreValidUtf8WithTheirUmlauts)
{
    set_language(Language::german);
    EXPECT_EQ(std::string(tr(Text::action_back)), "Zur\xC3\xBC"
                                                  "ck");
    EXPECT_EQ(std::string(tr(Text::shelf_recent)), "Neu hinzugef\xC3\xBC"
                                                   "gt");
    // No text may end in the middle of a multi-byte character.
    for (int i = 0; i < static_cast<int>(Text::count_); ++i)
    {
        const std::string text = tr(static_cast<Text>(i));
        int pending = 0;
        for (const char c : text)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (pending > 0)
            {
                ASSERT_EQ(byte & 0xC0, 0x80) << text;
                --pending;
            }
            else if (byte >= 0xF0)
                pending = 3;
            else if (byte >= 0xE0)
                pending = 2;
            else if (byte >= 0xC0)
                pending = 1;
            else
                ASSERT_LT(byte, 0x80) << text;
        }
        EXPECT_EQ(pending, 0) << text;
    }
}

TEST_F(I18nTest, TheConsoleDecidesAndEnglishIsTheFallback)
{
    EXPECT_EQ(from_console_language(4), Language::german);   // SCE_SYSTEM_PARAM_LANG_GERMAN
    EXPECT_EQ(from_console_language(1), Language::english);  // English (US)
    EXPECT_EQ(from_console_language(2), Language::english);  // French: not translated yet
    EXPECT_EQ(from_console_language(-1), Language::english); // the system did not say
    EXPECT_EQ(from_console_language(999), Language::english);
}

TEST_F(I18nTest, ParsesTheNamesAnOwnerMightWrite)
{
    Language out = Language::english;
    for (const char *name : {"de", "DE", "de-DE", "de_AT", "German", "deutsch"})
    {
        out = Language::english;
        EXPECT_TRUE(parse_language(name, &out)) << name;
        EXPECT_EQ(out, Language::german) << name;
    }
    for (const char *name : {"en", "en-US", "en_GB", "English"})
    {
        out = Language::german;
        EXPECT_TRUE(parse_language(name, &out)) << name;
        EXPECT_EQ(out, Language::english) << name;
    }
    out = Language::german;
    for (const char *name : {"", "auto", "fr", "dex", "d"})
    {
        EXPECT_FALSE(parse_language(name, &out)) << name;
        EXPECT_EQ(out, Language::german) << "unchanged for " << name;
    }
}

TEST_F(I18nTest, FormatsValuesAndCounts)
{
    set_language(Language::english);
    EXPECT_EQ(format(Text::time_left, "1:39:36"), "1:39:36 left");
    EXPECT_EQ(movie_count(1), "1 movie");
    EXPECT_EQ(movie_count(26), "26 movies");
    EXPECT_EQ(format(Text::decade, "2010"), "2010s");
    set_language(Language::german);
    EXPECT_EQ(format(Text::time_left, "1:39:36"), "noch 1:39:36");
    EXPECT_EQ(movie_count(1), "1 Film");
    EXPECT_EQ(movie_count(26), "26 Filme");
    EXPECT_EQ(format(Text::decade, "2010"), "2010er");
    EXPECT_EQ(format(Text::sorted_by, tr(Text::sort_year)), "Sortiert nach Jahr");
}

TEST_F(I18nTest, TheTmdbLanguageFollowsTheApp)
{
    using namespace hui::media;
    set_language(Language::english);
    EXPECT_EQ(config_from_env(parse_env("")).language, "en-US");
    set_language(Language::german);
    EXPECT_EQ(config_from_env(parse_env("")).language, "de-DE");
    // The owner's own choice wins.
    EXPECT_EQ(config_from_env(parse_env("TMDB_LANGUAGE=fr-FR")).language, "fr-FR");
    EXPECT_NE(std::string(config_from_env(parse_env("")).problem).find("TMDB_API_KEY"),
              std::string::npos);
}

TEST_F(I18nTest, DriveNamesAreTranslated)
{
    set_language(Language::german);
    EXPECT_EQ(hui::media::drive_label("/data/movies/x.mkv"), "Intern");
    EXPECT_EQ(hui::media::drive_label("/mnt/ext0/movies/x.mkv"), "Laufwerk 1");
    EXPECT_EQ(hui::media::drive_label("/mnt/usb2/movies/x.mkv"), "USB 3");
}

} // namespace
