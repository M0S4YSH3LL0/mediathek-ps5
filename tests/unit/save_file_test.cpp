// ps5-homebrew-ui - Save container and atomic write tests.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/save_file.hpp"
#include "core/settings.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

using hui::save::Kind;

TEST(SaveFile, Crc32MatchesReferenceVector)
{
    EXPECT_EQ(hui::save::crc32("123456789"), 0xcbf43926u);
    EXPECT_EQ(hui::save::crc32(""), 0u);
}

TEST(SaveFile, RoundTripsPayloadAndVersion)
{
    const std::string payload("game\0state", 10);
    const std::string encoded = hui::save::encode(Kind::game, 3, payload);
    const auto decoded = hui::save::decode(Kind::game, encoded);
    ASSERT_TRUE(decoded.ok) << decoded.error;
    EXPECT_EQ(decoded.version, 3);
    EXPECT_EQ(decoded.payload, payload);
}

TEST(SaveFile, RejectsCorruptionTruncationKindAndTrailingBytes)
{
    const std::string encoded = hui::save::encode(Kind::settings, 1, "volume=7");
    std::string flipped = encoded;
    flipped[14] ^= 0x01;
    EXPECT_FALSE(hui::save::decode(Kind::settings, flipped).ok);
    EXPECT_FALSE(hui::save::decode(Kind::settings, encoded.substr(0, encoded.size() - 1)).ok);
    EXPECT_FALSE(hui::save::decode(Kind::settings, encoded + "x").ok);
    EXPECT_FALSE(hui::save::decode(Kind::stats, encoded).ok);
    EXPECT_FALSE(hui::save::decode(Kind::settings, "HUIL").ok);
    EXPECT_FALSE(hui::save::decode(Kind::settings, "").ok);
}

TEST(SaveFile, WritesAtomicallyAndReadsBack)
{
    char directory[] = "/tmp/hui-save-XXXXXX";
    ASSERT_NE(mkdtemp(directory), nullptr);
    const std::string root = std::string(directory) + "/data";
    ASSERT_TRUE(hui::save::ensure_directory(root));
    ASSERT_TRUE(hui::save::ensure_directory(root));
    const std::string path = root + "/settings.bin";

    EXPECT_EQ(hui::save::write_atomic(path, "first"), "");
    EXPECT_EQ(hui::save::write_atomic(path, "second"), "");
    std::string data;
    ASSERT_TRUE(hui::save::read_file(path, &data));
    EXPECT_EQ(data, "second");
    EXPECT_NE(access((path + ".tmp").c_str(), F_OK), 0);
    EXPECT_FALSE(hui::save::read_file(root + "/missing.bin", &data));
    EXPECT_FALSE(hui::save::read_file(path, &data, 3));

    unlink(path.c_str());
    rmdir(root.c_str());
    rmdir(directory);
}

TEST(Settings, RoundTripsEveryField)
{
    hui::Settings settings;
    settings.music_volume = 3;
    settings.sfx_volume = 9;
    settings.ui_volume = 1;
    settings.reduced_motion = true;
    settings.swap_confirm = true;
    settings.show_fps = true;
    settings.haptics = false;
    settings.light_bar = false;
    settings.resolution = 1;
    settings.hint_size = 2;
    settings.concept_index = 7;
    settings.pin_billboard = false;
    hui::Settings read;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &read));
    EXPECT_EQ(read.music_volume, 3);
    EXPECT_EQ(read.sfx_volume, 9);
    EXPECT_EQ(read.ui_volume, 1);
    EXPECT_TRUE(read.reduced_motion);
    EXPECT_TRUE(read.swap_confirm);
    EXPECT_TRUE(read.show_fps);
    EXPECT_FALSE(read.haptics);
    EXPECT_FALSE(read.light_bar);
    EXPECT_EQ(read.resolution, 1);
    EXPECT_EQ(read.hint_size, 2);
    EXPECT_EQ(read.concept_index, 7);
    EXPECT_FALSE(read.pin_billboard);
    EXPECT_FLOAT_EQ(hui::Settings::gain(10), 1.0f);
    EXPECT_FLOAT_EQ(hui::Settings::gain(0), 0.0f);
}

// Files saved before the billboard setting existed hold one byte less and the old version.
TEST(Settings, AFileFromBeforeTheFixedBillboardStillLoadsWithItsDefault)
{
    std::string data = hui::encode_settings(hui::Settings{});
    data.pop_back(); // audio language
    data.pop_back(); // audio output
    data.pop_back(); // HDR output
    data.pop_back(); // the subtitle source
    data.pop_back(); // the subtitle language
    data.pop_back(); // forced subtitles
    data.pop_back(); // the shelves on Home
    data.pop_back(); // the wallpaper
    data.pop_back(); // the language
    data.pop_back(); // the billboard
    data[0] = 1;
    hui::Settings read;
    read.pin_billboard = false;
    read.music_volume = 2;
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.pin_billboard, hui::Settings{}.pin_billboard);
    EXPECT_TRUE(read.pin_billboard);
    EXPECT_EQ(read.music_volume, hui::Settings{}.music_volume);

    EXPECT_FALSE(hui::decode_settings(data.substr(0, data.size() - 1), &read)); // cut short
    data[0] = 2; // claims a newer version but lacks its byte
    EXPECT_FALSE(hui::decode_settings(data, &read));
}

TEST(Settings, TheWallpaperChoiceIsSavedAndAnOlderFileKeepsTheWallpaper)
{
    hui::Settings settings;
    ASSERT_TRUE(settings.details_wallpaper);
    settings.details_wallpaper = false;
    hui::Settings read;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &read));
    EXPECT_FALSE(read.details_wallpaper);

    std::string data = hui::encode_settings(settings);
    data.resize(data.size() - 8); // everything after the language setting
    data[0] = 3;                  // saved by the build with the language setting only
    read.details_wallpaper = false;
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_TRUE(read.details_wallpaper);
}

TEST(Settings, TheLanguageChoiceIsSavedAndAFileWithoutItMeansAutomatic)
{
    hui::Settings settings;
    settings.ui_language = 2;
    hui::Settings read;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &read));
    EXPECT_EQ(read.ui_language, 2);

    std::string data = hui::encode_settings(settings);
    data.resize(data.size() - 9); // the language setting and everything after it
    data[0] = 2;                  // saved by 0.6.0 and 0.6.1, before the language setting
    read.ui_language = 1;
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.ui_language, 0);

    data = hui::encode_settings(settings);
    data[data.size() - 9] = 9; // not a language
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.ui_language, 2);
}

TEST(Settings, TheHomeShelvesChoiceIsSavedAndAnOlderFileShowsAll)
{
    hui::Settings settings;
    ASSERT_EQ(settings.home_shelves, 0);
    settings.home_shelves = 2;
    hui::Settings read;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &read));
    EXPECT_EQ(read.home_shelves, 2);

    std::string data = hui::encode_settings(settings);
    data.resize(data.size() - 7); // the shelves on Home and everything after them
    data[0] = 4;                  // saved by 0.6.2
    read.home_shelves = 1;
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.home_shelves, 0);

    data = hui::encode_settings(settings);
    data[data.size() - 7] = 9; // not an option
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.home_shelves, 3);
}

TEST(Settings, TheSubtitleChoicesAreSavedAndAnOlderFileLeavesSubtitlesOff)
{
    hui::Settings settings;
    ASSERT_FALSE(settings.force_subtitles);
    ASSERT_EQ(settings.subtitle_language, 0);
    settings.force_subtitles = true;
    settings.subtitle_language = 1;
    hui::Settings read;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &read));
    EXPECT_TRUE(read.force_subtitles);
    EXPECT_EQ(read.subtitle_language, 1);

    std::string data = hui::encode_settings(settings);
    data.resize(data.size() - 6);
    data[0] = 5; // saved by 0.7.0
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_FALSE(read.force_subtitles);
    EXPECT_EQ(read.subtitle_language, 0);

    data = hui::encode_settings(settings);
    data[data.size() - 5] = 9; // not a language
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.subtitle_language, 2);
}

TEST(Settings, TheSubtitleSourceIsSavedAndAnOlderFileOffersBoth)
{
    hui::Settings settings;
    ASSERT_EQ(settings.subtitle_source, 0);
    settings.subtitle_source = 2;
    hui::Settings read;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &read));
    EXPECT_EQ(read.subtitle_source, 2);

    std::string data = hui::encode_settings(settings);
    data.resize(data.size() - 4);
    data[0] = 6; // saved by 0.8.0 and 0.9.0
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.subtitle_source, 0);

    data = hui::encode_settings(settings);
    data[data.size() - 4] = 9; // not a source
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.subtitle_source, 2);
}

TEST(Settings, HdrOutputIsSavedAndAnOlderFileIsAutomatic)
{
    hui::Settings settings;
    ASSERT_EQ(settings.hdr_output, 0);
    settings.hdr_output = 1;
    hui::Settings read;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &read));
    EXPECT_EQ(read.hdr_output, 1);

    std::string data = hui::encode_settings(settings);
    data.resize(data.size() - 3);
    data[0] = 7; // saved before HDR output
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.hdr_output, 0);

    data = hui::encode_settings(settings);
    data[data.size() - 3] = 9; // not an option
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.hdr_output, 1);
}

TEST(Settings, AudioOutputIsSavedAndAnOlderFileIsAutomatic)
{
    hui::Settings settings;
    ASSERT_EQ(settings.audio_output, 0);
    settings.audio_output = 1;
    hui::Settings read;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &read));
    EXPECT_EQ(read.audio_output, 1);

    std::string data = hui::encode_settings(settings);
    data.resize(data.size() - 2);
    data[0] = 8; // saved before the audio output setting
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.audio_output, 0);

    data = hui::encode_settings(settings);
    data[data.size() - 2] = 9; // not an option
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.audio_output, 1);
}

TEST(Settings, TheAudioLanguageIsSavedAndAnOlderFileIsAutomatic)
{
    hui::Settings settings;
    ASSERT_EQ(settings.audio_language, 0);
    settings.audio_language = 2;
    hui::Settings read;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &read));
    EXPECT_EQ(read.audio_language, 2);

    std::string data = hui::encode_settings(settings);
    data.pop_back();
    data[0] = 9; // saved before the audio language setting
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.audio_language, 0);

    data = hui::encode_settings(settings);
    data.back() = 9; // not a language
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.audio_language, 2);
    EXPECT_STREQ(hui::Settings::language_code(1), "ger");
    EXPECT_STREQ(hui::Settings::language_code(2), "eng");
    EXPECT_STREQ(hui::Settings::language_code(0), "");
}

TEST(Settings, ClampsOutOfRangeValuesAndRejectsGarbage)
{
    std::string data = hui::encode_settings(hui::Settings{});
    data[1] = 99;  // music volume
    data[9] = 99;  // resolution
    data[10] = 99; // hint size
    hui::Settings read;
    ASSERT_TRUE(hui::decode_settings(data, &read));
    EXPECT_EQ(read.music_volume, 10);
    EXPECT_EQ(read.resolution, hui::Settings::kResolutionCount - 1);
    EXPECT_EQ(read.hint_size, 2);

    hui::Settings untouched;
    untouched.music_volume = 2;
    EXPECT_FALSE(hui::decode_settings("", &untouched));
    EXPECT_FALSE(hui::decode_settings(data.substr(0, 5), &untouched));
    data[0] = 77; // unknown version
    EXPECT_FALSE(hui::decode_settings(data, &untouched));
    EXPECT_EQ(untouched.music_volume, 2);
}

TEST(SaveFile, ListFilesPrefersTheIndex)
{
    const std::string dir = ::testing::TempDir() + "hui_list_files";
    ASSERT_TRUE(hui::save::ensure_directory(dir));
    ::unlink((dir + "/index.txt").c_str());
    ASSERT_EQ(hui::save::write_atomic(dir + "/b.wav", "x"), "");
    ASSERT_EQ(hui::save::write_atomic(dir + "/a.wav", "x"), "");
    std::vector<std::string> listed = hui::save::list_files(dir);
    std::sort(listed.begin(), listed.end());
    EXPECT_EQ(listed, (std::vector<std::string>{"a.wav", "b.wav"}));

    ASSERT_EQ(hui::save::write_atomic(dir + "/index.txt", "one.ogg\r\ntwo.ogg\n\n../x\n"), "");
    EXPECT_EQ(hui::save::list_files(dir), (std::vector<std::string>{"one.ogg", "two.ogg"}));
    EXPECT_TRUE(hui::save::list_files(dir + "/missing").empty());
}

} // namespace
