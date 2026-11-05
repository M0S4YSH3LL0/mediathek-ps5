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
#include <vector>
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

TEST(MediaInfo, TheSoundLanguageIsReadFromTheTailOfAFilmName)
{
    using Codes = std::vector<std::string>;
    EXPECT_EQ(audio_languages_from_name("Blue Harbor (2011) German DL 1080p BluRay x264-GRP.mkv"),
              Codes{"ger"});
    EXPECT_EQ(audio_languages_from_name("Blue.Harbor.2011.GERMAN.DL.1080p.mkv"), Codes{"ger"});
    EXPECT_EQ(audio_languages_from_name("Blue Harbor (2011) [English].mkv"), Codes{"eng"});
    EXPECT_EQ(audio_languages_from_name("Blue Harbor (2011) Deutsch.mkv"), Codes{"ger"});
    EXPECT_EQ(audio_languages_from_name("Blue Harbor (2011) GER.mkv"), Codes{"ger"});
    EXPECT_EQ(audio_languages_from_name("Blue.Harbor.2011.German-GRP.mkv"), Codes{"ger"});
    EXPECT_EQ(audio_languages_from_name("Blue.Harbor.2011.German.English.DL.mkv"),
              (Codes{"ger", "eng"}));
    // No year to end the title: a language at its end still counts.
    EXPECT_EQ(audio_languages_from_name("Blue.Harbor.German.1080p.BluRay.x264-GRP.mkv"),
              Codes{"ger"});
    EXPECT_EQ(audio_languages_from_name("Blue Harbor German.mkv"), Codes{"ger"});
}

TEST(MediaInfo, ANameThatOnlyHasALanguageInItsTitleOrForSubtitlesSaysNothing)
{
    EXPECT_TRUE(audio_languages_from_name("The English Patient (1996).mkv").empty());
    EXPECT_TRUE(audio_languages_from_name("The German Doctor (2013) 1080p.mkv").empty());
    EXPECT_TRUE(audio_languages_from_name("Deutschland 83 (2015).mkv").empty());
    EXPECT_TRUE(audio_languages_from_name("It (2017).mkv").empty());
    EXPECT_TRUE(audio_languages_from_name("No (2012) DE.mkv").empty()); // two letters: too vague
    EXPECT_TRUE(audio_languages_from_name("Blue Harbor (2011) Eng.Sub.mkv").empty());
    EXPECT_TRUE(audio_languages_from_name("Blue Harbor (2011) German Subbed.mkv").empty());
    EXPECT_TRUE(audio_languages_from_name("Blue Harbor (2011).mkv").empty());
}

TEST(MediaInfo, TheFileNameStandsInOnlyWhereTheFileSaysNothing)
{
    const std::string name = "Blue Harbor (2011) German DL.mkv";
    MediaInfo untagged;
    untagged.audio_languages = {"und"};
    MediaInfo tagged;
    tagged.audio_languages = {"eng"};
    MediaInfo none;
    using Codes = std::vector<std::string>;
    // The file's tags win, even against the name.
    EXPECT_EQ(audio_languages_to_show(&tagged, true, name), Codes{"eng"});
    // Untagged, or without a sound track we could list, or unreadable: the name.
    EXPECT_EQ(audio_languages_to_show(&untagged, true, name), Codes{"ger"});
    EXPECT_EQ(audio_languages_to_show(&none, true, name), Codes{"ger"});
    EXPECT_EQ(audio_languages_to_show(nullptr, true, name), Codes{"ger"});
    // The name has nothing to add: what the file said stays.
    EXPECT_EQ(audio_languages_to_show(&untagged, true, "Blue Harbor (2011).mkv"), Codes{"und"});
    EXPECT_TRUE(audio_languages_to_show(nullptr, true, "Blue Harbor (2011).mkv").empty());
    // Not looked at yet: no guess that the reading would replace a moment later.
    EXPECT_TRUE(audio_languages_to_show(nullptr, false, name).empty());
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
    // A "Subs" folder names its files after the language alone.
    EXPECT_EQ(language_from_filename("English.srt"), "eng");
    EXPECT_EQ(language_from_filename("Deutsch.forced.srt"), "ger");
    EXPECT_EQ(language_from_filename("ger.srt"), "ger");
    EXPECT_EQ(language_from_filename("it.srt"), "");
}

namespace
{

// A few dozen lines of subtitle, the way an .srt has them, from a handful of sentences.
std::string srt_of(const std::vector<std::string> &lines)
{
    std::string text;
    int number = 1;
    for (int round = 0; round < 6; ++round)
        for (const std::string &line : lines)
        {
            text += std::to_string(number++) + "\n00:00:01,000 --> 00:00:03,000\n" + line + "\n\n";
        }
    return text;
}

} // namespace

TEST(MediaInfo, ASubtitleFileIsRecognisedByItsWords)
{
    EXPECT_EQ(language_from_text(srt_of({"I do not know what you are saying.",
                                         "But they will come with the others.",
                                         "This is just what you would have said."})),
              "eng");
    EXPECT_EQ(language_from_text(srt_of({"Ich habe das nicht gewusst, und du auch nicht.",
                                         "Wir sind mit der Zeit noch nicht fertig.",
                                         "Das ist ein Haus auf dem Berg, wie sie sagte."})),
              "ger");
    EXPECT_EQ(language_from_text(srt_of({"Je ne suis pas ici pour vous, mais pour elle.",
                                         "Nous sommes dans une maison avec des amis.",
                                         "Tu es tout ce qui est important, oui."})),
              "fre");
    EXPECT_EQ(language_from_text(srt_of({"Ik weet niet wat je bedoelt, maar het is mijn huis.",
                                         "Hij is gewoon een man van het dorp.",
                                         "Wij gaan naar een plek, dus kom."})),
              "dut");
}

TEST(MediaInfo, ASubtitleWithTooLittleToGoOnHasNoLanguage)
{
    EXPECT_EQ(language_from_text(""), "");
    EXPECT_EQ(language_from_text("1\n00:00:01,000 --> 00:00:03,000\nThe end.\n"), "");
    EXPECT_EQ(language_from_text(
                  srt_of({"\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"})),
              "");
    // A mix with no clear winner is not guessed.
    EXPECT_EQ(language_from_text(srt_of({"the and you that with have this what",
                                         "und der das nicht ich ist ein eine du sie"})),
              "");
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
    info.subtitle_languages = {"ger", "eng", "fre"};
    MediaInfo back;
    ASSERT_TRUE(decode_info(encode_info(info), &back));
    EXPECT_DOUBLE_EQ(back.duration, 8040.5);
    EXPECT_EQ(back.video_codec, "HEVC");
    EXPECT_EQ(back.width, 1920);
    EXPECT_EQ(back.height, 800);
    EXPECT_EQ(back.container, "MKV");
    EXPECT_EQ(back.audio_format, "E-AC3 5.1");
    EXPECT_EQ(back.audio_languages, info.audio_languages);
    EXPECT_EQ(back.subtitle_languages, info.subtitle_languages);

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
    EXPECT_FALSE(decode_info("mediainfo 4\nnot a pair\nend\n", &out));
    // A file of version 1 may list audio tracks the app cannot play: read the file again.
    EXPECT_FALSE(decode_info("mediainfo 1\naudio=eng,ger\nend\n", &out));
    // Version 2 never looked at the subtitle tracks inside the file.
    EXPECT_FALSE(decode_info("mediainfo 2\naudio=eng,ger\nend\n", &out));
    // Version 3 listed the languages of forced subtitle tracks, which the player leaves out.
    EXPECT_FALSE(decode_info("mediainfo 3\naudio=eng,ger\nend\n", &out));
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
