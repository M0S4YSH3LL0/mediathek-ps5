// Mediathek - Tests for what a movie file contains: languages, runtime, the cache text, the info
// service. Copyright (C) 2026 BlackBearReloaded SPDX-License-Identifier: GPL-3.0-or-later

#include "core/i18n.hpp"
#include "media/info_service.hpp"
#include "media/mediainfo.hpp"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace
{

using namespace hui::media;
using hui::i18n::Language;

// The texts follow the app's language: tests that read them set it and put it back.
class MediaInfoTexts : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        previous_ = hui::i18n::language();
        hui::i18n::set_language(Language::english);
    }
    void TearDown() override
    {
        hui::i18n::set_language(previous_);
    }

  private:
    Language previous_ = Language::english;
};

std::string temp_directory()
{
    char pattern[] = "/tmp/mediathek-info-XXXXXX";
    EXPECT_NE(mkdtemp(pattern), nullptr);
    return pattern;
}

bool wait_for(const std::function<bool()> &condition)
{
    for (int i = 0; i < 400; ++i)
    {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

TEST(MediaInfo, OneSpellingPerLanguage)
{
    for (const char *spelling : {"eng", "en", "English", "ENG", "en"})
        EXPECT_EQ(canonical_language(spelling), "eng") << spelling;
    for (const char *spelling : {"ger", "deu", "de", "Deutsch", "German"})
        EXPECT_EQ(canonical_language(spelling), "ger") << spelling;
    EXPECT_EQ(canonical_language("fra"), "fre");
    EXPECT_EQ(canonical_language("zho"), "chi");
    EXPECT_EQ(canonical_language("tlh"), "tlh"); // a code the table lacks stays as it is
    for (const char *nothing : {"", "und", "mis", "x", "12", "english language", "a-b"})
        EXPECT_EQ(canonical_language(nothing), "und") << nothing;
}

TEST_F(MediaInfoTexts, NamesFollowTheAppsLanguage)
{
    EXPECT_EQ(language_name("ger"), "German");
    EXPECT_EQ(language_name("deu"), "German");
    EXPECT_EQ(language_name("tlh"), "TLH");
    EXPECT_EQ(language_name("und"), "Unknown language");
    hui::i18n::set_language(Language::german);
    EXPECT_EQ(language_name("ger"), "Deutsch");
    EXPECT_EQ(language_name("fre"), "Franz\xC3\xB6sisch");
    EXPECT_EQ(genre_name(35), "Kom\xC3\xB6"
                              "die");
}

TEST_F(MediaInfoTexts, ListsShowUnknownOnlyWhenNothingElseIsKnown)
{
    EXPECT_TRUE(language_names({}).empty());
    EXPECT_EQ(language_names({"und"}), (std::vector<std::string>{"Unknown language"}));
    EXPECT_EQ(language_names({"eng", "und", "ger"}),
              (std::vector<std::string>{"English", "German"}));
}

TEST(MediaInfo, ALanguageComesFromTheTailOfASubtitleFileName)
{
    EXPECT_EQ(language_from_filename("Blue Harbor (2011).en.srt"), "eng");
    EXPECT_EQ(language_from_filename("Blue Harbor (2011).de.srt"), "ger");
    EXPECT_EQ(language_from_filename("2_English.srt"), "eng");
    EXPECT_EQ(language_from_filename("Film.German.forced.srt"), "ger");
    EXPECT_EQ(language_from_filename("Film (2011).srt"), "");
    // A film called It is not Italian, and one called No is not Norwegian.
    EXPECT_EQ(language_from_filename("It (2017).srt"), "");
    EXPECT_EQ(language_from_filename("It.srt"), "");
    EXPECT_EQ(language_from_filename("No (2012).srt"), "");
}

TEST(MediaInfo, TheRuntimeIsRoundedToTheMinute)
{
    EXPECT_EQ(format_runtime(8040.0), "2h 14m");
    EXPECT_EQ(format_runtime(8069.0), "2h 14m");
    EXPECT_EQ(format_runtime(8070.0), "2h 15m");
    EXPECT_EQ(format_runtime(2820.0), "47m");
    EXPECT_EQ(format_runtime(3600.0), "1h 00m");
    EXPECT_EQ(format_runtime(0.0), "");
    EXPECT_EQ(format_runtime(12.0), "");
}

TEST(MediaInfo, TheResolutionNamesTheUsualPictureSizes)
{
    EXPECT_EQ(resolution_label(1920, 1080), "1080p");
    EXPECT_EQ(resolution_label(1920, 800), "1080p"); // a scope film with bars
    EXPECT_EQ(resolution_label(3840, 1600), "4K");
    EXPECT_EQ(resolution_label(1280, 720), "720p");
    EXPECT_EQ(resolution_label(720, 404), "SD");
    EXPECT_EQ(resolution_label(0, 0), "");
}

TEST(MediaInfo, ComesBackFromItsCacheText)
{
    MediaInfo info;
    info.duration = 8040.5;
    info.video_codec = "HEVC";
    info.width = 1920;
    info.height = 800;
    info.container = "MKV";
    info.audio_format = "E-AC3 5.1";
    info.audio_languages = {"eng", "ger"};
    MediaInfo back;
    ASSERT_TRUE(decode_info(encode_info(info), &back));
    EXPECT_DOUBLE_EQ(back.duration, 8040.5);
    EXPECT_EQ(back.video_codec, "HEVC");
    EXPECT_EQ(back.width, 1920);
    EXPECT_EQ(back.height, 800);
    EXPECT_EQ(back.container, "MKV");
    EXPECT_EQ(back.audio_format, "E-AC3 5.1");
    EXPECT_EQ(back.audio_languages, info.audio_languages);

    MediaInfo empty_back;
    ASSERT_TRUE(decode_info(encode_info(MediaInfo()), &empty_back));
    EXPECT_TRUE(empty_back.audio_languages.empty());
}

TEST(MediaInfo, RefusesAHalfWrittenOrForeignCacheFile)
{
    MediaInfo out;
    EXPECT_FALSE(decode_info("", &out));
    EXPECT_FALSE(decode_info("hello\nend\n", &out));
    std::string text = encode_info(MediaInfo());
    EXPECT_FALSE(decode_info(text.substr(0, text.size() - 4), &out)); // no "end"
    EXPECT_FALSE(decode_info("mediainfo 2\nnot a pair\nend\n", &out));
    // A file of version 1 may list audio tracks the app cannot play: read the file again.
    EXPECT_FALSE(decode_info("mediainfo 1\naudio=eng,ger\nend\n", &out));
}

TEST(InfoService, ReadsAFileOnceAndKeepsTheAnswerOnDisk)
{
    const std::string directory = temp_directory();
    std::atomic<int> probes{0};
    const InfoService::Probe probe = [&probes](const std::string &path, MediaInfo *out)
    {
        ++probes;
        out->duration = 100.0;
        out->container = path;
        return true;
    };
    {
        InfoService service(probe, directory);
        EXPECT_EQ(service.peek("a1").state, InfoService::State::unknown);
        service.request("a1", "/movies/a.mkv");
        ASSERT_TRUE(
            wait_for([&] { return service.peek("a1").state == InfoService::State::ready; }));
        EXPECT_EQ(service.peek("a1").info->container, "/movies/a.mkv");
        service.request("a1", "/movies/a.mkv"); // known: no second read
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(probes.load(), 1);
    }
    // A later start finds the answer in the cache, without opening the file.
    InfoService again(probe, directory);
    again.request("a1", "/movies/a.mkv");
    ASSERT_TRUE(wait_for([&] { return again.peek("a1").state == InfoService::State::ready; }));
    EXPECT_EQ(probes.load(), 1);
    EXPECT_EQ(again.peek("a1").info->container, "/movies/a.mkv");
}

TEST(InfoService, AFileThatCannotBeReadFailsAndIsTriedAgainWhenAskedAgain)
{
    std::atomic<bool> readable{false};
    InfoService service(
        [&readable](const std::string &, MediaInfo *out)
        {
            out->duration = 5.0 * 60.0;
            return readable.load();
        },
        temp_directory());
    service.request("b2", "/movies/b.mkv");
    ASSERT_TRUE(wait_for([&] { return service.peek("b2").state == InfoService::State::failed; }));
    EXPECT_EQ(service.peek("b2").info, nullptr);
    readable = true; // the drive woke up
    service.request("b2", "/movies/b.mkv");
    ASSERT_TRUE(wait_for([&] { return service.peek("b2").state == InfoService::State::ready; }));
    EXPECT_DOUBLE_EQ(service.peek("b2").info->duration, 300.0);
}

TEST(InfoService, RefreshReadsTheFileAgainAndKeepsTheOldAnswerUntilThen)
{
    std::atomic<int> version{1};
    std::atomic<bool> hold{false};
    InfoService service(
        [&](const std::string &, MediaInfo *out)
        {
            while (hold.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            out->duration = version.load();
            return true;
        },
        temp_directory());
    service.request("c3", "/movies/c.mkv");
    ASSERT_TRUE(wait_for([&] { return service.peek("c3").state == InfoService::State::ready; }));
    EXPECT_DOUBLE_EQ(service.peek("c3").info->duration, 1.0);
    version = 2;
    hold = true;
    service.refresh("c3", "/movies/c.mkv");
    const InfoService::Lookup during = service.peek("c3");
    EXPECT_EQ(during.state, InfoService::State::pending);
    ASSERT_NE(during.info, nullptr); // the old answer is still there to show
    EXPECT_DOUBLE_EQ(during.info->duration, 1.0);
    hold = false;
    ASSERT_TRUE(wait_for([&] { return service.peek("c3").state == InfoService::State::ready; }));
    EXPECT_DOUBLE_EQ(service.peek("c3").info->duration, 2.0);
}

} // namespace
