// Mediathek - Tests for the screens: browsing, the details page, playback and resume points.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"

#include "concepts/concepts.hpp"
#include "media/services.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>

namespace
{

using hui::Action;
using hui::action_bit;
using hui::Direction;
using hui::InputFrame;
namespace app = hui::app;
namespace audio = hui::audio;
namespace concepts = hui::concepts;
namespace media = hui::media;

using hui::i18n::Text;
using hui::i18n::tr;

class MediathekTest : public hui::testing::ConceptFixture
{
  protected:
    MediathekTest()
    {
        surface_.create = [](int, int, const std::uint8_t *) { return std::uint32_t{7}; };
        surface_.update = [](std::uint32_t, int, int, const std::uint8_t *) {};
        surface_.release = [](std::uint32_t) {};
        player_ = media::make_simulated_player(surface_, 7200.0);
        services_.player = player_.get();
        services_.clock = [this] { return clock_seconds_; };
        context_.media = &services_;
    }

    // A player whose films have subtitle tracks of their own (see make_simulated_player):
    // English and German text, then German pictures for the hard of hearing.
    void use_embedded_subtitles()
    {
        player_ = media::make_simulated_player(surface_, 7200.0, false, true);
        services_.player = player_.get();
    }

    void fill_library()
    {
        media::ScanResult scan;
        scan.roots = {"/data/movies"};
        const char *names[] = {"Blue Harbor (2011).mkv", "Copper Skies (2020) 2160p.mkv",
                               "Night Train to Nowhere (1987).mkv", "Paper Moons (2015).mp4"};
        std::uint64_t size = 1ull << 30;
        for (const char *name : names)
        {
            media::FoundFile file;
            file.path = std::string("/data/movies/") + name;
            file.readable = file.path;
            file.size = size;
            file.mtime = 1700000000 + static_cast<std::int64_t>(size >> 20);
            scan.videos.push_back(file);
            size += 1ull << 28;
        }
        services_.library = media::Library::from_scan(scan);
        ++services_.revision;
    }

    std::unique_ptr<app::Concept> make()
    {
        auto design = concepts::make_mediathek(context_);
        design->enter();
        send(*design, idle(), 1.0f);
        return design;
    }

    // The first shelf is Continue Watching, so the page has a second shelf to go down to.
    void add_second_shelf()
    {
        services_.progress.update(services_.library[0].id, 100.0, 7200.0, clock_seconds_);
    }

    // The white Play pill of the billboard, as drawn: 64 high, 214 or 250 wide, solid.
    int play_pills(const app::Concept &design)
    {
        frame_.reset();
        design.draw(frame_);
        int pills = 0;
        for (const hui::gfx::Instance &instance : frame_.scene.instances())
        {
            const bool size = instance.rect[3] == 64.0f &&
                              (instance.rect[2] == 214.0f || instance.rect[2] == 250.0f);
            pills += size && instance.color_top[3] > 0.95f ? 1 : 0;
        }
        return pills;
    }

    // How many shelves Home has: up to the first (a rebuild keeps the focus on the same film,
    // wherever that shelf now is), then down until the page refuses.
    int count_shelves(app::Concept &design)
    {
        do
            send(design, nav(Direction::up), 0.3f);
        while (played(audio::Cue::tab));
        int shelves = 1;
        for (;;)
        {
            send(design, nav(Direction::down), 0.3f);
            if (!played(audio::Cue::tab))
                break;
            ++shelves;
        }
        return shelves;
    }

    // A library with every kind of shelf: Continue Watching, All Movies, a collection, and two
    // genres. Decades and Recently Added need more films, so a small one does not show them.
    void fill_library_with_genres()
    {
        fill_library();
        add_second_shelf();
        int index = 0;
        services_.library.edit_all(
            [&index](media::Movie *movie)
            {
                if (index < 2)
                    movie->genres = {index == 0 ? 28 : 18};
                else
                    movie->collection = "Saga";
                ++index;
            });
        ++services_.revision;
    }

    // Twelve films, six each from two decades: enough for the decade shelves, and no genres.
    void fill_library_with_decades()
    {
        media::ScanResult scan;
        scan.roots = {"/data/movies"};
        for (int i = 0; i < 12; ++i)
        {
            media::FoundFile file;
            file.path = "/data/movies/Film " + std::to_string(i) + " (" +
                        std::to_string(i < 6 ? 1990 + i : 2000 + i) + ").mkv";
            file.readable = file.path;
            file.size = 1ull << 30;
            file.mtime = 1700000000 + i;
            scan.videos.push_back(file);
        }
        services_.library = media::Library::from_scan(scan);
        ++services_.revision;
    }

    // One film with an English and a German subtitle file, in that order.
    void fill_library_with_subtitles()
    {
        media::ScanResult scan;
        scan.roots = {"/data/movies"};
        media::FoundFile video;
        video.path = "/data/movies/Blue Harbor (2011).mkv";
        video.readable = video.path;
        video.size = 1ull << 30;
        scan.videos.push_back(video);
        for (const char *name : {"Blue Harbor (2011).en.srt", "Blue Harbor (2011).de.srt"})
        {
            media::FoundFile sub;
            sub.path = std::string("/data/movies/") + name;
            sub.readable = sub.path;
            scan.subtitles.push_back(sub);
        }
        services_.library = media::Library::from_scan(scan);
        ++services_.revision;
    }

    // What Triangle's message says for subtitle file `language`, number `step` of two.
    float subtitle_toast(Text language, int step)
    {
        return subtitle_toast({language, Text::subtitles_srt}, step, 2);
    }
    // ... and for any track: "Deutsch · SDH · embedded (2/5)".
    float subtitle_toast(std::initializer_list<Text> parts, int step, int count)
    {
        std::string label;
        for (const Text part : parts)
            label += (label.empty() ? "" : " \xC2\xB7 ") + std::string(tr(part));
        label += " (" + std::to_string(step) + "/" + std::to_string(count) + ")";
        return toast_width_of(hui::i18n::format(Text::subtitles_track, label));
    }

    // The toast as drawn: its pill is 56 high with its top at 906 once it has arrived. -1: none.
    float toast_width(const app::Concept &design)
    {
        frame_.reset();
        design.draw(frame_);
        for (const hui::gfx::Instance &instance : frame_.scene.instances())
        {
            if (instance.rect[3] == 56.0f && instance.rect[1] == 906.0f)
                return instance.rect[2];
        }
        return -1.0f;
    }

    float toast_width_of(const std::string &text)
    {
        return context_.fonts.semibold.measure(text, 24) + 64;
    }

    // Library, the reload row, the question, then "Reload".
    void start_reload(app::Concept &design)
    {
        open_category(design, kLibrary);
        send(design, press(Action::confirm), 0.6f);
        send(design, nav(Direction::right), 0.3f);
        send(design, press(Action::confirm), 0.0f);
    }

    // The settings list, top to bottom (SettingsCategory in mediathek.cpp).
    static constexpr int kAppearance = 0; // Fixed billboard, Shelves on Home, Wallpaper
    static constexpr int kPlayback = 1;   // Force subtitles, Subtitle language
    static constexpr int kGeneral = 2;    // App language, UI sounds
    static constexpr int kLibrary = 3;    // Reload all
    static constexpr int kAbout = 4;      // Version, Author (read-only), Debug info

    // The settings tab with the focus in the form of this category (the order of the list).
    void open_category(app::Concept &design, int category)
    {
        // Movies, Series, Settings.
        send(design, press(Action::page_next), 0.5f);
        send(design, press(Action::page_next), 0.5f);
        send(design, press(Action::page_next), 0.5f);
        for (int i = 0; i < category; ++i)
            send(design, nav(Direction::down), 0.3f);
        send(design, nav(Direction::right), 0.3f);
    }

    // The four films and one series of two seasons, two episodes each, in a tvshows folder.
    void fill_library_with_series()
    {
        media::ScanResult scan;
        scan.roots = {"/data/movies", "/data/tvshows"};
        const char *films[] = {"Blue Harbor (2011).mkv", "Paper Moons (2015).mp4"};
        for (const char *name : films)
        {
            media::FoundFile file;
            file.path = std::string("/data/movies/") + name;
            file.readable = file.path;
            file.size = 1ull << 30;
            file.mtime = 1700000000;
            scan.videos.push_back(file);
        }
        const char *episodes[] = {"Season 1/Tide.S01E01.mkv", "Season 1/Tide.S01E02.mkv",
                                  "Season 2/Tide.S02E01.mkv", "Season 2/Tide.S02E02.mkv"};
        for (const char *name : episodes)
        {
            media::FoundFile file;
            file.path = std::string("/data/tvshows/Tide/") + name;
            file.readable = file.path;
            file.size = 1ull << 28;
            file.mtime = 1700000000;
            scan.videos.push_back(file);
        }
        services_.library = media::Library::from_scan(scan);
        ++services_.revision;
    }

    // The id of episode `episode` of season `season` (both counted from 1) of the series.
    std::string episode_id(int season, int episode) const
    {
        for (const media::Movie &movie : services_.library.movies())
            if (movie.show)
                return movie.seasons[static_cast<std::size_t>(season - 1)]
                    .episodes[static_cast<std::size_t>(episode - 1)]
                    .id;
        return {};
    }

    // The Series tab: Home, Movies, Series.
    void open_series(app::Concept &design)
    {
        send(design, press(Action::page_next), 0.5f);
        send(design, press(Action::page_next), 0.5f);
    }

    media::Services services_;
    media::VideoSurface surface_;
    std::unique_ptr<media::Player> player_;
    std::int64_t clock_seconds_ = 1700001000;
};

TEST_F(MediathekTest, AnEmptyLibraryDrawsAndOffersToScanAgain)
{
    int scans = 0;
    services_.rescan = [&scans] { ++scans; };
    auto design = make();
    expect_drawable(*design);
    send(*design, press(Action::menu));
    EXPECT_EQ(scans, 1);
}

TEST_F(MediathekTest, ABusyScanIsNotStartedTwice)
{
    int scans = 0;
    services_.rescan = [&scans] { ++scans; };
    services_.scanning = true;
    auto design = make();
    expect_drawable(*design);
    send(*design, press(Action::menu));
    EXPECT_EQ(scans, 0);
}

TEST_F(MediathekTest, BrowsingMakesSoundAndStaysDrawable)
{
    fill_library();
    auto design = make();
    expect_drawable(*design);
    send(*design, nav(Direction::right), 0.3f);
    EXPECT_TRUE(played(audio::Cue::focus));
    send(*design, nav(Direction::down), 0.6f);
    expect_drawable(*design);
}

TEST_F(MediathekTest, CrossOpensTheDetailsSheetAndCircleClosesIt)
{
    fill_library();
    auto design = make();
    send(*design, press(Action::confirm));
    EXPECT_TRUE(played(audio::Cue::open));
    expect_drawable(*design);
    send(*design, press(Action::back));
    expect_drawable(*design);
    // Back on the shelves, Cross opens it again: the sheet really closed.
    send(*design, press(Action::confirm));
    EXPECT_TRUE(played(audio::Cue::open));
}

TEST_F(MediathekTest, TheDetailsPageStepsToTheNeighbouringFilmAndRefusesAtTheEnds)
{
    fill_library();
    auto design = make();
    send(*design, press(Action::confirm));
    send(*design, press(Action::page_next), 0.5f);
    EXPECT_TRUE(played(audio::Cue::focus));
    expect_drawable(*design);
    send(*design, nav(Direction::left), 0.5f);
    EXPECT_TRUE(played(audio::Cue::focus));
    send(*design, nav(Direction::left), 0.5f); // already on the first film
    EXPECT_TRUE(played(audio::Cue::error));
    EXPECT_FALSE(played(audio::Cue::focus));
    expect_drawable(*design);
    // Still on the page: Circle closes it, a second Circle is the staged exit.
    send(*design, press(Action::back));
    EXPECT_TRUE(played(audio::Cue::back));
}

TEST_F(MediathekTest, TriangleCyclesThroughSubtitleFilesAndThenOff)
{
    fill_library_with_subtitles();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    for (int i = 0; i < 4; ++i) // English, German, off, English again
    {
        send(*design, press(Action::north), 0.3f);
        EXPECT_TRUE(played(audio::Cue::toggle));
        expect_drawable(*design);
    }
}

TEST_F(MediathekTest, AFilmStartsWithoutSubtitlesAndTriangleFindsThePreferredLanguageFirst)
{
    settings_.subtitle_language = 1; // German
    fill_library_with_subtitles();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    send(*design, press(Action::north), 0.5f); // off before: the first press switches them on
    EXPECT_EQ(toast_width(*design), subtitle_toast(Text::lang_ger, 1));
    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(toast_width(*design), subtitle_toast(Text::lang_eng, 2));
    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(toast_width(*design), toast_width_of(tr(Text::subtitles_off)));
}

TEST_F(MediathekTest, ForcedSubtitlesStartInThePreferredLanguage)
{
    settings_.force_subtitles = true;
    settings_.subtitle_language = 1; // German
    fill_library_with_subtitles();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    send(*design, press(Action::north), 0.5f); // German was on: the next one is English
    EXPECT_EQ(toast_width(*design), subtitle_toast(Text::lang_eng, 2));
    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(toast_width(*design), toast_width_of(tr(Text::subtitles_off)));
}

TEST_F(MediathekTest, ForcedSubtitlesWithoutAPreferenceStartOnTheFirstFile)
{
    settings_.force_subtitles = true;
    fill_library_with_subtitles();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    send(*design, press(Action::north), 0.5f); // English (the library's first) was on
    EXPECT_EQ(toast_width(*design), subtitle_toast(Text::lang_ger, 2));
}

// The film's own tracks come before the files in each language, and Force subtitles waits for
// the player to know them. Entries: en.srt, de.srt, then English, German, German SDH pictures.
TEST_F(MediathekTest, ForcedSubtitlesPreferTheFilmsOwnTrackToAFile)
{
    settings_.force_subtitles = true;
    settings_.subtitle_language = 1; // German
    use_embedded_subtitles();
    fill_library_with_subtitles();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    EXPECT_EQ(player_->subtitle_track(), 1) << "the German text track inside the film";
    EXPECT_FALSE(player_->subtitle_text().empty());
    expect_drawable(*design);

    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(
        toast_width(*design),
        subtitle_toast({Text::lang_ger, Text::subtitles_sdh, Text::subtitles_embedded}, 2, 5));
    EXPECT_EQ(player_->subtitle_track(), 2);
    EXPECT_NE(player_->subtitle_picture().texture, 0u) << "a picture track draws a picture";
    EXPECT_TRUE(player_->subtitle_text().empty());
    expect_drawable(*design);

    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(toast_width(*design), subtitle_toast({Text::lang_ger, Text::subtitles_srt}, 3, 5));
    EXPECT_EQ(player_->subtitle_track(), -1) << "a file is the app's to show, not the player's";
    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(toast_width(*design),
              subtitle_toast({Text::lang_eng, Text::subtitles_embedded}, 4, 5));
    EXPECT_EQ(player_->subtitle_track(), 0);
}

TEST_F(MediathekTest, AFilmWithOnlyItsOwnSubtitlesStillSwitchesThemOn)
{
    use_embedded_subtitles();
    fill_library(); // no files beside the films
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    // No full subtitles unless forced on, but the forced track in the sound's language.
    EXPECT_EQ(player_->subtitle_track(), 3);
    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(toast_width(*design),
              subtitle_toast({Text::lang_eng, Text::subtitles_embedded}, 1, 3));
    EXPECT_EQ(player_->subtitle_track(), 0);
}

// The simulated film has an English forced track (3) and English and German sound.
TEST_F(MediathekTest, AForcedTrackShowsWhileNoFullSubtitlesAreOn)
{
    use_embedded_subtitles();
    fill_library();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    EXPECT_EQ(player_->subtitle_track(), 3) << "the sound is English";
    // German sound: there is no German forced track.
    send(*design, press(Action::west), 0.5f);
    EXPECT_EQ(player_->subtitle_track(), -1);
    send(*design, press(Action::west), 0.5f);
    EXPECT_EQ(player_->subtitle_track(), 3);
    // Triangle steps through the three full tracks only, and off brings the forced one back.
    for (const int full : {0, 1, 2})
    {
        send(*design, press(Action::north), 0.5f);
        EXPECT_EQ(player_->subtitle_track(), full);
    }
    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(player_->subtitle_track(), 3);
}

TEST_F(MediathekTest, AForcedTrackFollowsTheSubtitleLanguageFirst)
{
    settings_.subtitle_language = 1; // German: the film has no German forced track
    use_embedded_subtitles();
    fill_library();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    EXPECT_EQ(player_->subtitle_track(), -1) << "not the English one the sound would pick";
}

TEST_F(MediathekTest, SubtitleSourceEmbeddedOnlyLeavesTheFilesOut)
{
    settings_.force_subtitles = true;
    settings_.subtitle_language = 1; // German
    settings_.subtitle_source = 1;   // embedded only
    use_embedded_subtitles();
    fill_library_with_subtitles();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    EXPECT_EQ(player_->subtitle_track(), 1);
    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(
        toast_width(*design),
        subtitle_toast({Text::lang_ger, Text::subtitles_sdh, Text::subtitles_embedded}, 2, 3));
    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(toast_width(*design),
              subtitle_toast({Text::lang_eng, Text::subtitles_embedded}, 3, 3));
    EXPECT_EQ(player_->subtitle_track(), 0);
}

TEST_F(MediathekTest, SubtitleSourceSrtOnlyLeavesTheFilmsOwnTracksOut)
{
    settings_.force_subtitles = true;
    settings_.subtitle_language = 1; // German
    settings_.subtitle_source = 2;   // .srt only
    use_embedded_subtitles();
    fill_library_with_subtitles();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    EXPECT_EQ(player_->subtitle_track(), -1) << "the German file, not the film's track";
    send(*design, press(Action::north), 0.5f);
    EXPECT_EQ(toast_width(*design), subtitle_toast({Text::lang_eng, Text::subtitles_srt}, 2, 2));
    EXPECT_EQ(player_->subtitle_track(), -1);
}

TEST_F(MediathekTest, TheSubtitleSettingsAreAFormOfTheirOwn)
{
    auto design = make();
    open_category(*design, kPlayback);
    context_.settings_changed = false;
    send(*design, press(Action::confirm), 0.4f); // Force subtitles
    EXPECT_TRUE(settings_.force_subtitles);
    EXPECT_TRUE(context_.settings_changed);
    send(*design, nav(Direction::down), 0.3f); // Preferred language: None
    send(*design, nav(Direction::right), 0.3f);
    EXPECT_EQ(settings_.subtitle_language, 1); // German
    send(*design, nav(Direction::right), 0.3f);
    EXPECT_EQ(settings_.subtitle_language, 2); // English
    send(*design, nav(Direction::down), 0.3f); // Subtitle source: Automatic
    send(*design, nav(Direction::right), 0.3f);
    EXPECT_EQ(settings_.subtitle_source, 1); // embedded only
    send(*design, nav(Direction::right), 0.3f);
    EXPECT_EQ(settings_.subtitle_source, 2); // .srt only
}

TEST_F(MediathekTest, TheTabKeysSwitchBetweenHomeAndTheMovieGrid)
{
    fill_library();
    auto design = make();
    send(*design, press(Action::page_next), 0.8f);
    expect_drawable(*design);
    send(*design, nav(Direction::down), 0.4f);
    expect_drawable(*design);
    send(*design, press(Action::north), 0.4f); // the sort order, not a watched mark
    expect_drawable(*design);
    send(*design, press(Action::page_prev), 0.8f);
    expect_drawable(*design);
}

TEST_F(MediathekTest, R3SortsTheMovieGridAndTriangleOpensTheSearch)
{
    fill_library();
    auto design = make();
    send(*design, press(Action::page_next), 0.8f);
    send(*design, press(Action::r3), 0.4f);
    EXPECT_TRUE(played(audio::Cue::tab));
    send(*design, press(Action::north), 0.4f);
    EXPECT_TRUE(played(audio::Cue::modal_open)); // the keyboard, not a sort
    expect_drawable(*design);
    // While it is open, the tab keys and R3 belong to the keyboard.
    send(*design, press(Action::r3), 0.2f);
    EXPECT_FALSE(played(audio::Cue::tab));
    send(*design, press(Action::back), 0.5f); // closes the keyboard only
    EXPECT_TRUE(played(audio::Cue::modal_close));
    expect_drawable(*design);
}

TEST_F(MediathekTest, ASearchFiltersTheGridAndBackClearsItBeforeLeavingThePage)
{
    fill_library();
    auto design = make();
    send(*design, press(Action::page_next), 0.8f);
    send(*design, press(Action::north), 0.4f);
    // The keyboard opens on "g" (capital: the field starts a text): "Gu" matches nothing here.
    send(*design, press(Action::confirm), 0.2f);
    send(*design, press(Action::confirm), 0.2f);
    send(*design, press(Action::menu), 0.5f); // Done
    expect_drawable(*design);
    EXPECT_FALSE(played(audio::Cue::error));
    // Nothing is on the grid now, yet the page still answers: Back clears the search ...
    send(*design, press(Action::back), 0.4f);
    EXPECT_TRUE(played(audio::Cue::back));
    // ... and the next Back leaves the page for Home (no exit warning yet).
    send(*design, press(Action::back), 0.4f);
    EXPECT_TRUE(played(audio::Cue::back));
    send(*design, press(Action::confirm), 0.3f);
    EXPECT_TRUE(played(audio::Cue::open)); // Home again: its films open the details
}

TEST_F(MediathekTest, BackLeavesTheAppOnlyOnASecondPressAtTheTop)
{
    fill_library();
    int quits = 0;
    services_.quit = [&quits] { ++quits; };
    auto design = make();

    send(*design, press(Action::back), 0.3f); // at the first film already: asks
    EXPECT_EQ(quits, 0);
    send(*design, idle(), 3.5f); // the question lapses
    send(*design, press(Action::back), 0.3f);
    EXPECT_EQ(quits, 0);
    send(*design, press(Action::back), 0.3f); // answered within the time
    EXPECT_EQ(quits, 1);
}

TEST_F(MediathekTest, BackStepsOutOfTheGridAndBackToTheFirstFilmBeforeExiting)
{
    fill_library();
    int quits = 0;
    services_.quit = [&quits] { ++quits; };
    auto design = make();
    send(*design, press(Action::page_next), 0.8f);
    send(*design, press(Action::back), 0.8f); // grid to Home
    EXPECT_EQ(quits, 0);
    send(*design, nav(Direction::right), 0.4f);
    send(*design, press(Action::back), 0.4f); // back to the first film
    EXPECT_EQ(quits, 0);
    send(*design, press(Action::back), 0.3f);
    EXPECT_EQ(quits, 0); // only the question so far
    send(*design, press(Action::back), 0.3f);
    EXPECT_EQ(quits, 1);
}

TEST_F(MediathekTest, BackExitsFromAnEmptyLibraryToo)
{
    int quits = 0;
    services_.quit = [&quits] { ++quits; };
    auto design = make();
    send(*design, press(Action::back), 0.3f);
    send(*design, press(Action::back), 0.3f);
    EXPECT_EQ(quits, 1);
}

TEST_F(MediathekTest, TriangleOnTheHomePageTogglesWatched)
{
    fill_library();
    auto design = make();
    send(*design, nav(Direction::right));
    int watched_before = 0;
    for (const media::Movie &m : services_.library.movies())
        watched_before += services_.progress.watched(m.id) ? 1 : 0;
    send(*design, press(Action::north));
    int watched_after = 0;
    for (const media::Movie &m : services_.library.movies())
        watched_after += services_.progress.watched(m.id) ? 1 : 0;
    EXPECT_EQ(watched_after, watched_before + 1);
    send(*design, press(Action::north));
    int watched_again = 0;
    for (const media::Movie &m : services_.library.movies())
        watched_again += services_.progress.watched(m.id) ? 1 : 0;
    EXPECT_EQ(watched_again, watched_before);
}

TEST_F(MediathekTest, PlayingAMovieAndLeavingItKeepsAResumePoint)
{
    fill_library();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    EXPECT_EQ(player_->state(), media::PlayerState::playing);
    expect_drawable(*design);
    send(*design, idle(), 30.0f); // past the minimum resume position
    send(*design, press(Action::back), 0.5f);
    EXPECT_NE(player_->state(), media::PlayerState::playing);

    int resumable = 0;
    for (const media::Movie &m : services_.library.movies())
        resumable += services_.progress.in_progress(m.id) ? 1 : 0;
    EXPECT_EQ(resumable, 1);
    EXPECT_TRUE(services_.progress.dirty());
    expect_drawable(*design);
}

TEST_F(MediathekTest, CrossPausesAndSeekingMovesThePosition)
{
    fill_library();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    send(*design, press(Action::confirm), 0.3f);
    EXPECT_EQ(player_->state(), media::PlayerState::paused);
    const double before = player_->position();
    send(*design, nav(Direction::right), 0.3f);
    EXPECT_GT(player_->position(), before + 5.0);
    send(*design, press(Action::confirm), 0.3f);
    EXPECT_EQ(player_->state(), media::PlayerState::playing);
    expect_drawable(*design);
}

// On an HDR10 screen the film is put under the interface by a pass of its own (main.cpp,
// gfx::HdrComposer): the player screen names its picture and leaves it, and the black behind
// it, out of the draw list, so the interface layer stays clear where the film shows.
TEST_F(MediathekTest, ThePlayerNamesItsPictureAndLeavesItToTheHdrPass)
{
    fill_library();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    ASSERT_EQ(player_->state(), media::PlayerState::playing);
    const auto picture_runs = [this]
    {
        int runs = 0;
        for (const hui::gfx::Run &run : frame_.scene.runs())
            runs += run.texture == player_->texture() ? 1 : 0;
        return runs;
    };
    expect_drawable(*design);
    EXPECT_EQ(frame_.video.texture, player_->texture());
    EXPECT_GT(frame_.video.rect.w, 0.0f);
    EXPECT_GT(picture_runs(), 0);

    services_.hdr_output = true;
    expect_drawable(*design);
    EXPECT_EQ(frame_.video.texture, player_->texture());
    EXPECT_EQ(picture_runs(), 0);
}

TEST_F(MediathekTest, HoldingASeekDirectionGrowsTheStepToFiveMinutes)
{
    fill_library();
    media::VideoSurface surface;
    surface.create = [](int, int, const std::uint8_t *) { return std::uint32_t{7}; };
    surface.update = [](std::uint32_t, int, int, const std::uint8_t *) {};
    surface.release = [](std::uint32_t) {};
    player_ = media::make_simulated_player(surface, 100000.0); // long enough not to clamp the jumps
    services_.player = player_.get();
    auto design = make();
    send(*design, press(Action::west), 2.0f);
    send(*design, press(Action::confirm), 0.3f); // paused: the position only moves by seeks
    double last = player_->position();
    send(*design, nav(Direction::right), 0.35f);
    EXPECT_NEAR(player_->position() - last, 10.0, 0.01); // a tap
    InputFrame held = nav(Direction::right);
    held.nav_repeat = true;
    double biggest = 0.0;
    double previous_step = 0.0;
    for (int i = 0; i < 120; ++i) // 13 s of repeats at the console's 0.11 s rate
    {
        last = player_->position();
        send(*design, held, 0.11f);
        const double step = player_->position() - last;
        EXPECT_GE(step, previous_step); // never shrinks while held
        previous_step = step;
        biggest = std::max(biggest, step);
    }
    EXPECT_NEAR(biggest, 300.0, 0.01);
    send(*design, InputFrame{}, 1.0f); // let go ...
    last = player_->position();
    send(*design, nav(Direction::right), 0.3f);
    EXPECT_NEAR(player_->position() - last, 10.0, 0.01); // ... and a tap is 10 s again
    expect_drawable(*design);
}

TEST_F(MediathekTest, AnyInputInAnyStateStaysDrawable)
{
    fill_library();
    auto design = make();
    std::uint32_t seed = 12345;
    const auto next = [&seed]
    {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    const Direction directions[] = {Direction::none, Direction::up, Direction::down,
                                    Direction::left, Direction::right};
    const Action actions[] = {Action::confirm,   Action::back,      Action::north,
                              Action::west,      Action::page_prev, Action::page_next,
                              Action::jump_prev, Action::jump_next, Action::menu};
    for (int i = 0; i < 600; ++i)
    {
        InputFrame input = idle();
        input.nav = directions[next() % 5];
        if (next() % 3 == 0)
            input.pressed = action_bit(actions[next() % 9]);
        input.held = input.pressed;
        feedback_.clear();
        design->update(input, 1.0f / 30.0f, feedback_);
        if (i % 50 == 0)
            expect_drawable(*design);
    }
    expect_drawable(*design);
}

// The gear: Home, Movies, Series, Settings are one row of tabs for L1 and R1.
TEST_F(MediathekTest, TheSettingsTabIsReachedWithTheTabKeysAndLeftWithBack)
{
    fill_library();
    auto design = make();
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::page_next), 0.5f);
    expect_drawable(*design);
    send(*design, press(Action::page_next), 0.3f); // the end of the row: nothing happens
    EXPECT_FALSE(played(audio::Cue::tab));
    send(*design, press(Action::back), 0.5f);
    EXPECT_TRUE(played(audio::Cue::back));
    send(*design, press(Action::page_next), 0.5f); // Back went to Home, so this is Movies
    EXPECT_TRUE(played(audio::Cue::tab));
}

TEST_F(MediathekTest, TheTogglesChangeTheSettingsAtOnceAndSayThatTheyChanged)
{
    auto design = make(); // works without a single film
    ASSERT_GT(settings_.ui_volume, 0);
    ASSERT_FALSE(settings_.show_fps);
    open_category(*design, kAppearance);

    ASSERT_TRUE(settings_.pin_billboard); // on unless the viewer turned it off
    context_.settings_changed = false;
    send(*design, press(Action::confirm), 0.4f); // fixed billboard
    EXPECT_FALSE(settings_.pin_billboard);
    EXPECT_TRUE(context_.settings_changed);

    send(*design, nav(Direction::down), 0.3f); // Shelves on Home
    send(*design, nav(Direction::down), 0.3f);
    context_.settings_changed = false;
    send(*design, press(Action::confirm), 0.4f); // the wallpaper of the details page
    EXPECT_FALSE(settings_.details_wallpaper);
    EXPECT_TRUE(context_.settings_changed);
    EXPECT_EQ(settings_.home_shelves, 0);

    send(*design, press(Action::back), 0.3f);  // to the list
    send(*design, nav(Direction::down), 0.3f); // Playback
    send(*design, nav(Direction::down), 0.3f); // General
    send(*design, nav(Direction::right), 0.3f);
    send(*design, nav(Direction::down), 0.3f); // past App language
    context_.settings_changed = false;
    send(*design, press(Action::confirm), 0.4f); // UI sounds
    EXPECT_EQ(settings_.ui_volume, 0);
    EXPECT_TRUE(context_.settings_changed);
    send(*design, press(Action::confirm), 0.4f);
    EXPECT_EQ(settings_.ui_volume, hui::Settings{}.ui_volume);

    send(*design, press(Action::back), 0.3f);
    send(*design, nav(Direction::down), 0.3f); // Library
    send(*design, nav(Direction::down), 0.3f); // About
    send(*design, nav(Direction::right), 0.3f);
    context_.settings_changed = false;
    send(*design, press(Action::confirm), 0.4f); // debug info
    EXPECT_TRUE(settings_.show_fps);
    EXPECT_TRUE(context_.settings_changed);
    expect_drawable(*design);
}

TEST_F(MediathekTest, TheSettingsListStartsWithAppearanceAndHasUiSoundsUnderGeneral)
{
    auto design = make();
    open_category(*design, kGeneral);
    send(*design, nav(Direction::down), 0.3f);   // App language, then UI sounds
    send(*design, press(Action::confirm), 0.4f); // UI sounds
    EXPECT_EQ(settings_.ui_volume, 0);
    EXPECT_EQ(settings_.ui_language, 0);
    EXPECT_TRUE(settings_.pin_billboard);
}

// The two panes: the list on the left only chooses, the form on the right changes things.
TEST_F(MediathekTest, TheCategoryListChangesNothingUntilTheFormHasTheFocus)
{
    auto design = make();
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::page_next), 0.5f);
    context_.settings_changed = false;
    send(*design, nav(Direction::left), 0.3f); // the list is the leftmost pane
    send(*design, nav(Direction::down), 0.3f);
    EXPECT_TRUE(played(audio::Cue::focus));
    send(*design, nav(Direction::up), 0.3f);
    send(*design, press(Action::confirm), 0.3f); // opens the form, flips nothing
    EXPECT_TRUE(settings_.pin_billboard);
    EXPECT_FALSE(context_.settings_changed);
    send(*design, press(Action::confirm), 0.3f); // now it does
    EXPECT_FALSE(settings_.pin_billboard);
}

TEST_F(MediathekTest, LeftAndBackLeaveTheFormWithoutChangingASwitch)
{
    auto design = make();
    open_category(*design, kAppearance); // Fixed billboard, on
    context_.settings_changed = false;
    send(*design, nav(Direction::left), 0.3f);
    EXPECT_TRUE(played(audio::Cue::back));
    EXPECT_TRUE(settings_.pin_billboard);
    EXPECT_FALSE(context_.settings_changed);

    // Down moves the list, so the focus really is there, and the form follows it.
    send(*design, nav(Direction::down), 0.3f); // Playback
    send(*design, nav(Direction::down), 0.3f); // General
    send(*design, nav(Direction::right), 0.3f);
    send(*design, nav(Direction::down), 0.3f); // UI sounds
    send(*design, press(Action::back), 0.3f);  // Back: to the list, not yet to Home
    send(*design, press(Action::confirm), 0.3f);
    send(*design, press(Action::confirm), 0.3f);
    EXPECT_EQ(settings_.ui_volume, 0); // the row the form was left on: still on the page
    EXPECT_EQ(settings_.ui_language, 0);
    EXPECT_TRUE(settings_.pin_billboard);

    // A switch does not answer right either: Cross is the one way to change it.
    context_.settings_changed = false;
    send(*design, nav(Direction::right), 0.3f);
    EXPECT_FALSE(context_.settings_changed);
    send(*design, nav(Direction::left), 0.3f);
    EXPECT_FALSE(context_.settings_changed);
    EXPECT_EQ(settings_.ui_volume, 0);
}

TEST_F(MediathekTest, BackFromTheListLeavesTheSettingsAndComingBackStartsAtTheList)
{
    auto design = make();
    open_category(*design, kAppearance);
    send(*design, press(Action::back), 0.3f); // the list
    send(*design, press(Action::back), 0.5f); // Home
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::confirm), 0.3f); // Appearance's form, not a switch
    EXPECT_TRUE(settings_.pin_billboard);

    // L1 out of the form, then back: the list has the focus again.
    send(*design, press(Action::page_prev), 0.5f);
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::confirm), 0.3f);
    EXPECT_TRUE(settings_.pin_billboard);
    send(*design, press(Action::confirm), 0.3f);
    EXPECT_FALSE(settings_.pin_billboard);
}

TEST_F(MediathekTest, TheHomeShelvesSettingChoosesWhichShelvesHomeShows)
{
    fill_library_with_genres();
    auto design = make();
    EXPECT_EQ(count_shelves(*design), 5); // continue, the collection, two genres, all

    settings_.home_shelves = 1;
    ++services_.revision;
    send(*design, idle(), 0.1f);
    EXPECT_EQ(count_shelves(*design), 3); // continue, the collection, all

    settings_.home_shelves = 2;
    ++services_.revision;
    send(*design, idle(), 0.1f);
    EXPECT_EQ(count_shelves(*design), 3); // continue, the two genres
    expect_drawable(*design);
}

TEST_F(MediathekTest, TheDecadeShelvesFollowTheHomeShelvesSetting)
{
    fill_library_with_decades();
    auto design = make();
    EXPECT_EQ(count_shelves(*design), 4); // recently added, two decades, all

    const auto choose = [&](int option)
    {
        settings_.home_shelves = option;
        ++services_.revision;
        send(*design, idle(), 0.1f);
    };
    choose(1);
    EXPECT_EQ(count_shelves(*design), 2); // recently added, all: no decades
    choose(2);
    EXPECT_EQ(count_shelves(*design), 1); // no genres and no decades: all, as the fallback
    choose(3);
    EXPECT_EQ(count_shelves(*design), 2); // the two decades

    add_second_shelf();
    ++services_.revision;
    send(*design, idle(), 0.1f);
    EXPECT_EQ(count_shelves(*design), 3); // continue, the two decades
    expect_drawable(*design);
}

// A library that has both: each choice shows its own kind and not the other's.
TEST_F(MediathekTest, GenresOnlyHasNoDecadesAndDecadesOnlyHasNoGenres)
{
    fill_library_with_decades();
    int index = 0;
    services_.library.edit_all([&index](media::Movie *movie)
                               { movie->genres = {index++ < 6 ? 28 : 18}; });
    ++services_.revision;
    settings_.home_shelves = 2;
    auto design = make();
    EXPECT_EQ(count_shelves(*design), 2); // the two genres; with decades it would be four

    settings_.home_shelves = 3;
    ++services_.revision;
    send(*design, idle(), 0.1f);
    EXPECT_EQ(count_shelves(*design), 2); // the two decades

    settings_.home_shelves = 0;
    ++services_.revision;
    send(*design, idle(), 0.1f);
    EXPECT_EQ(count_shelves(*design), 6); // recently added, genres, decades, all
}

TEST_F(MediathekTest, DecadesOnlyInASmallLibraryStillShowsItsFilms)
{
    fill_library();
    settings_.home_shelves = 3;
    auto design = make();
    EXPECT_EQ(count_shelves(*design), 1); // under twelve films there are no decade shelves
}

TEST_F(MediathekTest, GenresOnlyStillHasAShelfWhenNoFilmHasAGenre)
{
    fill_library();
    settings_.home_shelves = 2;
    auto design = make();
    EXPECT_EQ(count_shelves(*design), 1); // all the films, with nothing to continue

    add_second_shelf();
    ++services_.revision;
    send(*design, idle(), 0.1f);
    EXPECT_EQ(count_shelves(*design), 2); // continue alone would hide the other films
    EXPECT_GE(play_pills(*design), 1);    // a film is focused, the billboard has its Play
    send(*design, press(Action::confirm), 0.4f);
    expect_drawable(*design);
}

TEST_F(MediathekTest, TheHomeShelvesRowChangesHomeAtOnce)
{
    fill_library_with_genres();
    auto design = make();
    open_category(*design, kAppearance);
    send(*design, nav(Direction::down), 0.3f); // Shelves on Home
    context_.settings_changed = false;
    send(*design, nav(Direction::right), 0.3f); // All -> own collections
    EXPECT_EQ(settings_.home_shelves, 1);
    EXPECT_TRUE(context_.settings_changed);
    send(*design, press(Action::confirm), 0.3f); // -> genres
    EXPECT_EQ(settings_.home_shelves, 2);
    send(*design, press(Action::back), 0.3f);
    send(*design, press(Action::back), 0.5f); // Home
    EXPECT_EQ(count_shelves(*design), 3);     // continue, the two genres
}

TEST_F(MediathekTest, HoldingRightIntoTheFormDoesNotChangeTheLanguage)
{
    auto design = make();
    open_category(*design, kGeneral); // the press that opened it is still held
    InputFrame held = nav(Direction::right);
    held.nav_repeat = true;
    send(*design, held, 0.2f);
    send(*design, held, 0.2f);
    EXPECT_EQ(settings_.ui_language, 0);
    send(*design, nav(Direction::right), 0.3f); // a new press does
    EXPECT_EQ(settings_.ui_language, 1);
    held = nav(Direction::right);
    held.nav_repeat = true;
    send(*design, held, 0.3f); // and from then on holding it goes on
    EXPECT_EQ(settings_.ui_language, 2);
    hui::i18n::set_language(hui::i18n::Language::english);
}

TEST_F(MediathekTest, TheAboutFormSkipsItsReadOnlyVersionAndAuthorRows)
{
    auto design = make();
    open_category(*design, kAbout); // opens on Debug info, below the two read-only rows
    expect_drawable(*design);
    send(*design, nav(Direction::up), 0.3f); // nothing focusable above Debug info
    EXPECT_FALSE(played(audio::Cue::focus));
    EXPECT_TRUE(played(audio::Cue::error));
    send(*design, nav(Direction::down), 0.3f); // nor below
    EXPECT_FALSE(played(audio::Cue::focus));
    EXPECT_TRUE(played(audio::Cue::error));
    send(*design, press(Action::confirm), 0.3f);
    EXPECT_TRUE(settings_.show_fps); // still on Debug info
}

TEST_F(MediathekTest, TheBillboardCollapsesBelowTheFirstShelfUnlessItIsFixed)
{
    fill_library();
    add_second_shelf();
    settings_.pin_billboard = false;
    auto design = make();
    ASSERT_EQ(play_pills(*design), 1); // open on the first shelf
    send(*design, nav(Direction::down), 1.0f);
    EXPECT_EQ(play_pills(*design), 0);
    send(*design, nav(Direction::up), 1.0f);
    EXPECT_EQ(play_pills(*design), 1);

    settings_.pin_billboard = true;
    send(*design, nav(Direction::down), 1.0f);
    EXPECT_EQ(play_pills(*design), 1); // stays while the shelves scroll
    expect_drawable(*design);
    send(*design, nav(Direction::up), 1.0f);
    EXPECT_EQ(play_pills(*design), 1);
}

TEST_F(MediathekTest, TheBillboardIsFixedByDefault)
{
    EXPECT_TRUE(hui::Settings{}.pin_billboard);
    fill_library();
    add_second_shelf();
    auto design = make();
    send(*design, nav(Direction::down), 1.0f);
    EXPECT_EQ(play_pills(*design), 1);
}

TEST_F(MediathekTest, ASmallMessageSaysHowManyMoviesAScanFound)
{
    fill_library();
    auto design = make();
    send(*design, idle(), 0.1f);
    EXPECT_FALSE(played(audio::Cue::notify));
    services_.scan_found = 4;
    send(*design, idle(), 0.1f);
    EXPECT_TRUE(played(audio::Cue::notify)); // whatever the Debug info setting says
    EXPECT_EQ(services_.scan_found, -1);     // said once
    send(*design, idle(), 0.1f);
    EXPECT_FALSE(played(audio::Cue::notify));
    expect_drawable(*design);
}

TEST_F(MediathekTest, TheLanguageRowSwitchesTheAppAtOnce)
{
    namespace i18n = hui::i18n;
    services_.automatic_language = i18n::Language::german; // a German console
    i18n::set_language(i18n::Language::german);
    auto design = make();
    open_category(*design, kGeneral);
    ASSERT_EQ(settings_.ui_language, 0);

    context_.settings_changed = false;
    send(*design, nav(Direction::right), 0.4f); // Automatic -> English
    EXPECT_EQ(settings_.ui_language, 1);
    EXPECT_EQ(i18n::language(), i18n::Language::english);
    EXPECT_TRUE(context_.settings_changed);
    expect_drawable(*design);

    send(*design, nav(Direction::right), 0.4f); // -> Deutsch
    EXPECT_EQ(settings_.ui_language, 2);
    EXPECT_EQ(i18n::language(), i18n::Language::german);

    send(*design, nav(Direction::left), 0.4f);
    send(*design, nav(Direction::left), 0.4f); // back to Automatic: the console's language
    EXPECT_EQ(settings_.ui_language, 0);
    EXPECT_EQ(i18n::language(), i18n::Language::german);
    // The page still has its focus on the row after the texts were rebuilt.
    send(*design, nav(Direction::right), 0.4f);
    EXPECT_EQ(settings_.ui_language, 1);
    expect_drawable(*design);
    i18n::set_language(i18n::Language::english);
}

TEST_F(MediathekTest, TheSettingsPageShowsWhatWasSaved)
{
    settings_.ui_volume = 0;
    settings_.show_fps = true;
    settings_.pin_billboard = true;
    auto design = make();
    open_category(*design, kAppearance);
    send(*design, press(Action::confirm), 0.4f);
    EXPECT_FALSE(settings_.pin_billboard);
    send(*design, press(Action::back), 0.3f);
    send(*design, nav(Direction::down), 0.3f); // Playback
    send(*design, nav(Direction::down), 0.3f); // General
    send(*design, nav(Direction::right), 0.3f);
    send(*design, nav(Direction::down), 0.3f);   // UI sounds
    send(*design, press(Action::confirm), 0.4f); // flips "off" to on, not on to off
    EXPECT_EQ(settings_.ui_volume, hui::Settings{}.ui_volume);
    send(*design, press(Action::back), 0.3f);
    send(*design, nav(Direction::down), 0.3f); // Library
    send(*design, nav(Direction::down), 0.3f); // About
    send(*design, nav(Direction::right), 0.3f);
    send(*design, press(Action::confirm), 0.4f);
    EXPECT_FALSE(settings_.show_fps);
}

TEST_F(MediathekTest, ReloadingEverythingAsksFirstThenFetchesEveryFilmAgain)
{
    fill_library();
    int fetched = 0;
    services_.refetch_cover = [&fetched](const std::string &)
    {
        ++fetched;
        return std::string();
    };
    auto design = make();
    open_category(*design, kLibrary);
    send(*design, press(Action::confirm), 0.6f);
    EXPECT_TRUE(played(audio::Cue::modal_open));
    expect_drawable(*design);
    EXPECT_EQ(fetched, 0);

    send(*design, press(Action::confirm), 0.6f); // "Cancel" has the focus
    EXPECT_EQ(fetched, 0);

    send(*design, press(Action::confirm), 0.6f);
    send(*design, nav(Direction::right), 0.3f);
    send(*design, press(Action::confirm), 0.6f);
    EXPECT_EQ(fetched, 4);
    expect_drawable(*design);
}

// The console's queue, played by hand: services_.cover_jobs is what main.cpp sets every frame.
TEST_F(MediathekTest, AReloadCountsItsProgressInAMessageThatStaysUntilItIsDone)
{
    namespace i18n = hui::i18n;
    fill_library();
    services_.refetch_cover = [](const std::string &) { return std::string(); };
    auto design = make();
    services_.cover_jobs = 8; // films still being fetched: part of the work to wait for
    start_reload(*design);
    EXPECT_TRUE(played(audio::Cue::notify));
    services_.cover_jobs = 12;
    send(*design, idle(), 0.5f);
    EXPECT_FALSE(played(audio::Cue::notify)); // one sound for the whole reload
    EXPECT_EQ(toast_width(*design), toast_width_of(i18n::format(Text::reload_progress, "1/12")));

    services_.cover_jobs = 2;
    send(*design, idle(), 10.0f); // far longer than a message lasts
    EXPECT_EQ(toast_width(*design), toast_width_of(i18n::format(Text::reload_progress, "11/12")));

    services_.cover_jobs = 0;
    send(*design, idle(), 0.0f);
    EXPECT_TRUE(played(audio::Cue::notify));
    send(*design, idle(), 1.0f);
    EXPECT_EQ(toast_width(*design), toast_width_of(tr(Text::reload_finished)));
    send(*design, idle(), 3.0f);
    EXPECT_EQ(toast_width(*design), -1.0f); // and the finished message goes away
    expect_drawable(*design);
}

TEST_F(MediathekTest, AReloadThatIsGivenUpOnDoesNotSayItFinished)
{
    fill_library();
    services_.refetch_cover = [](const std::string &) { return std::string(); };
    auto design = make();
    services_.cover_jobs = 4;
    start_reload(*design);
    send(*design, idle(), 0.5f);

    services_.cover_jobs = 0;
    services_.cover_note = "TMDB cannot be reached";
    send(*design, idle(), 0.0f);
    EXPECT_FALSE(played(audio::Cue::notify));
    send(*design, idle(), 2.5f); // a problem's message arrives late: it is up for longer
    EXPECT_EQ(toast_width(*design), toast_width_of("TMDB cannot be reached"));
}

TEST_F(MediathekTest, AnotherMessageIsNotOverwrittenByAReloadInProgress)
{
    fill_library();
    int scans = 0;
    services_.rescan = [&scans] { ++scans; };
    services_.refetch_cover = [](const std::string &) { return std::string(); };
    auto design = make();
    services_.cover_jobs = 4;
    start_reload(*design);
    send(*design, idle(), 0.5f);

    send(*design, press(Action::menu), 0.5f); // "Looking for media..."
    EXPECT_EQ(scans, 1);
    EXPECT_EQ(toast_width(*design), toast_width_of(tr(Text::looking_for_media)));
    send(*design, idle(), 4.0f);
    EXPECT_EQ(toast_width(*design),
              toast_width_of(hui::i18n::format(Text::reload_progress, "5/8")));
}

TEST_F(MediathekTest, AReloadStaysQuietWhileAFilmPlays)
{
    fill_library();
    services_.refetch_cover = [](const std::string &) { return std::string(); };
    auto design = make();
    services_.cover_jobs = 4;
    start_reload(*design);
    send(*design, idle(), 3.0f);

    send(*design, press(Action::back), 0.5f); // the list
    send(*design, press(Action::back), 0.5f); // Home
    send(*design, press(Action::west), 3.0f); // Square: play, and the message runs out
    EXPECT_EQ(toast_width(*design), -1.0f);
    services_.cover_jobs = 0;
    send(*design, idle(), 0.5f);
    EXPECT_FALSE(played(audio::Cue::notify));
    EXPECT_EQ(toast_width(*design), -1.0f);

    send(*design, press(Action::back), 0.0f); // stop: now it says so
    send(*design, idle(), 0.0f);
    EXPECT_TRUE(played(audio::Cue::notify));
}

TEST_F(MediathekTest, BackClosesTheReloadQuestionBeforeItLeavesTheSettings)
{
    fill_library();
    int fetched = 0;
    services_.refetch_cover = [&fetched](const std::string &)
    {
        ++fetched;
        return std::string();
    };
    auto design = make();
    open_category(*design, kLibrary);
    send(*design, press(Action::confirm), 0.6f);
    send(*design, press(Action::back), 0.6f);
    EXPECT_TRUE(played(audio::Cue::modal_close));
    EXPECT_EQ(fetched, 0);
    send(*design, press(Action::back), 0.5f); // now the form, to the category list
    EXPECT_TRUE(played(audio::Cue::back));
    send(*design, press(Action::back), 0.5f); // and the page itself
    EXPECT_TRUE(played(audio::Cue::back));
}

TEST_F(MediathekTest, AReloadThatIsRefusedStopsAtTheFirstFilm)
{
    fill_library();
    int asked = 0;
    services_.refetch_cover = [&asked](const std::string &)
    {
        ++asked;
        return std::string("No TMDB_API_KEY in mediathek.env");
    };
    auto design = make();
    open_category(*design, kLibrary);
    send(*design, press(Action::confirm), 0.6f);
    send(*design, nav(Direction::right), 0.3f);
    send(*design, press(Action::confirm), 0.6f);
    EXPECT_EQ(asked, 1);
    EXPECT_TRUE(played(audio::Cue::error));
}

TEST_F(MediathekTest, ReloadingAnEmptyLibraryJustSaysThereIsNothing)
{
    int asked = 0;
    services_.refetch_cover = [&asked](const std::string &)
    {
        ++asked;
        return std::string();
    };
    auto design = make();
    open_category(*design, kLibrary);
    send(*design, press(Action::confirm), 0.6f);
    EXPECT_TRUE(played(audio::Cue::error));
    EXPECT_EQ(asked, 0);
}

TEST_F(MediathekTest, TheSeriesTabPlaysASeriesFromItsFirstEpisode)
{
    fill_library_with_series();
    EXPECT_EQ(services_.library.show_count(), 1u);
    auto design = make();
    open_series(*design);
    expect_drawable(*design);
    send(*design, press(Action::west), 30.0f); // Square: play, past the resume threshold
    EXPECT_EQ(player_->state(), media::PlayerState::playing);
    expect_drawable(*design);
    send(*design, press(Action::back), 0.5f);
    EXPECT_TRUE(services_.progress.in_progress(episode_id(1, 1)));
    // The series' own id is no file and has no resume point.
    for (const media::Movie &movie : services_.library.movies())
        if (movie.show)
            EXPECT_FALSE(services_.progress.in_progress(movie.id));
    expect_drawable(*design);
}

TEST_F(MediathekTest, TheDetailsPageChoosesTheSeasonAndTheEpisodeToPlay)
{
    fill_library_with_series();
    auto design = make();
    open_series(*design);
    send(*design, press(Action::confirm), 0.8f);
    expect_drawable(*design);
    send(*design, nav(Direction::down), 0.3f);  // the Season row
    send(*design, nav(Direction::right), 0.4f); // Season 2
    EXPECT_TRUE(played(audio::Cue::tick));
    send(*design, nav(Direction::right), 0.4f); // there is no Season 3
    EXPECT_TRUE(played(audio::Cue::error));
    send(*design, nav(Direction::down), 0.3f);  // the Episode row
    send(*design, nav(Direction::right), 0.4f); // Episode 2
    expect_drawable(*design);
    send(*design, press(Action::confirm), 30.0f);
    EXPECT_EQ(player_->state(), media::PlayerState::playing);
    send(*design, press(Action::back), 0.5f);
    EXPECT_TRUE(services_.progress.in_progress(episode_id(2, 2)));
    EXPECT_FALSE(services_.progress.in_progress(episode_id(1, 1)));
}

TEST_F(MediathekTest, R1InThePlayerStartsTheNextEpisode)
{
    fill_library_with_series();
    auto design = make();
    open_series(*design);
    send(*design, press(Action::west), 30.0f);
    send(*design, press(Action::page_next), 30.0f);
    EXPECT_EQ(player_->state(), media::PlayerState::playing);
    EXPECT_LT(player_->position(), 60.0); // from the start, not where the last one was
    send(*design, press(Action::back), 0.5f);
    EXPECT_TRUE(services_.progress.in_progress(episode_id(1, 1))); // kept where it was left
    EXPECT_TRUE(services_.progress.in_progress(episode_id(1, 2)));
}

TEST_F(MediathekTest, TheEndOfAnEpisodeStartsTheNextOneAcrossTheSeason)
{
    fill_library_with_series();
    // Season 1's last episode is under way.
    services_.progress.update(episode_id(1, 2), 600.0, 7200.0, clock_seconds_);
    ++services_.revision;
    auto design = make();
    open_series(*design);
    send(*design, press(Action::west), 1.0f); // resumes S1 E2
    EXPECT_EQ(player_->state(), media::PlayerState::playing);
    player_->seek_to(7200.0 - 30.0); // the "Up next" card's last seconds
    send(*design, idle(), 1.0f);
    expect_drawable(*design);
    send(*design, idle(), 60.0f); // it ends, and Season 2 starts
    EXPECT_TRUE(services_.progress.watched(episode_id(1, 2)));
    EXPECT_EQ(player_->state(), media::PlayerState::playing);
    send(*design, press(Action::back), 0.5f);
    EXPECT_TRUE(services_.progress.in_progress(episode_id(2, 1)));
}

TEST_F(MediathekTest, TheLastEpisodeEndsThePlayback)
{
    fill_library_with_series();
    services_.progress.update(episode_id(2, 2), 600.0, 7200.0, clock_seconds_);
    ++services_.revision;
    auto design = make();
    open_series(*design);
    send(*design, press(Action::west), 1.0f); // resumes S2 E2, the last
    player_->seek_to(7200.0 - 5.0);
    send(*design, idle(), 30.0f);
    EXPECT_TRUE(services_.progress.watched(episode_id(2, 2)));
    EXPECT_NE(player_->state(), media::PlayerState::playing);
    expect_drawable(*design);
}

} // namespace
