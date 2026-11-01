// Mediathek - Tests for the screens: browsing, the details page, playback and resume points.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"

#include "concepts/concepts.hpp"
#include "media/services.hpp"

#include <cstdint>
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

class MediathekTest : public hui::testing::ConceptFixture
{
  protected:
    MediathekTest()
    {
        media::VideoSurface surface;
        surface.create = [](int, int, const std::uint8_t *) { return std::uint32_t{7}; };
        surface.update = [](std::uint32_t, int, int, const std::uint8_t *) {};
        surface.release = [](std::uint32_t) {};
        player_ = media::make_simulated_player(surface, 7200.0);
        services_.player = player_.get();
        services_.clock = [this] { return clock_seconds_; };
        context_.media = &services_;
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

    // The settings tab with the focus in the form of this category (the order of the list).
    void open_category(app::Concept &design, int category)
    {
        send(design, press(Action::page_next), 0.5f);
        send(design, press(Action::page_next), 0.5f);
        for (int i = 0; i < category; ++i)
            send(design, nav(Direction::down), 0.3f);
        send(design, nav(Direction::right), 0.3f);
    }

    media::Services services_;
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

// The gear: Home, Movies, Settings are one row of tabs for L1 and R1.
TEST_F(MediathekTest, TheSettingsTabIsReachedWithTheTabKeysAndLeftWithBack)
{
    fill_library();
    auto design = make();
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
    open_category(*design, 0); // Sound

    context_.settings_changed = false;
    send(*design, press(Action::confirm), 0.4f); // UI sounds
    EXPECT_EQ(settings_.ui_volume, 0);
    EXPECT_TRUE(context_.settings_changed);
    send(*design, press(Action::confirm), 0.4f);
    EXPECT_EQ(settings_.ui_volume, hui::Settings{}.ui_volume);

    send(*design, press(Action::back), 0.3f);  // to the list
    send(*design, nav(Direction::down), 0.3f); // Appearance
    send(*design, nav(Direction::right), 0.3f);
    ASSERT_TRUE(settings_.pin_billboard); // on unless the viewer turned it off
    context_.settings_changed = false;
    send(*design, press(Action::confirm), 0.4f); // fixed billboard
    EXPECT_FALSE(settings_.pin_billboard);
    EXPECT_TRUE(context_.settings_changed);

    send(*design, nav(Direction::down), 0.3f);
    context_.settings_changed = false;
    send(*design, press(Action::confirm), 0.4f); // the wallpaper of the details page
    EXPECT_FALSE(settings_.details_wallpaper);
    EXPECT_TRUE(context_.settings_changed);

    send(*design, press(Action::back), 0.3f);
    send(*design, nav(Direction::down), 0.3f); // Language
    send(*design, nav(Direction::down), 0.3f); // Library
    send(*design, nav(Direction::down), 0.3f); // Developer
    send(*design, nav(Direction::right), 0.3f);
    context_.settings_changed = false;
    send(*design, press(Action::confirm), 0.4f); // debug info
    EXPECT_TRUE(settings_.show_fps);
    EXPECT_TRUE(context_.settings_changed);
    expect_drawable(*design);
}

// The two panes: the list on the left only chooses, the form on the right changes things.
TEST_F(MediathekTest, TheCategoryListChangesNothingUntilTheFormHasTheFocus)
{
    auto design = make();
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::page_next), 0.5f);
    context_.settings_changed = false;
    send(*design, nav(Direction::left), 0.3f); // the list is the leftmost pane
    send(*design, nav(Direction::down), 0.3f);
    EXPECT_TRUE(played(audio::Cue::focus));
    send(*design, nav(Direction::up), 0.3f);
    send(*design, press(Action::confirm), 0.3f); // opens the form, flips nothing
    EXPECT_GT(settings_.ui_volume, 0);
    EXPECT_FALSE(context_.settings_changed);
    send(*design, press(Action::confirm), 0.3f); // now it does
    EXPECT_EQ(settings_.ui_volume, 0);
}

TEST_F(MediathekTest, LeftAndBackLeaveTheFormWithoutChangingASwitch)
{
    auto design = make();
    open_category(*design, 0); // Sound: UI sounds, on
    context_.settings_changed = false;
    send(*design, nav(Direction::left), 0.3f);
    EXPECT_TRUE(played(audio::Cue::back));
    EXPECT_GT(settings_.ui_volume, 0);
    EXPECT_FALSE(context_.settings_changed);

    // Down moves the list, so the focus really is there, and the form follows it.
    send(*design, nav(Direction::down), 0.3f);
    send(*design, nav(Direction::right), 0.3f);
    send(*design, press(Action::back), 0.3f); // Back: to the list, not yet to Home
    send(*design, press(Action::confirm), 0.3f);
    send(*design, press(Action::confirm), 0.3f);
    EXPECT_FALSE(settings_.pin_billboard); // Appearance's first row: still on the page

    // A switch does not answer right either: Cross is the one way to change it.
    context_.settings_changed = false;
    send(*design, nav(Direction::right), 0.3f);
    EXPECT_FALSE(context_.settings_changed);
    send(*design, nav(Direction::left), 0.3f);
    EXPECT_FALSE(context_.settings_changed);
    EXPECT_FALSE(settings_.pin_billboard);
}

TEST_F(MediathekTest, BackFromTheListLeavesTheSettingsAndComingBackStartsAtTheList)
{
    auto design = make();
    open_category(*design, 0);
    send(*design, press(Action::back), 0.3f); // the list
    send(*design, press(Action::back), 0.5f); // Home
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::confirm), 0.3f); // Sound's form, not a switch
    EXPECT_GT(settings_.ui_volume, 0);

    // L1 out of the form, then back: the list has the focus again.
    send(*design, press(Action::page_prev), 0.5f);
    send(*design, press(Action::page_next), 0.5f);
    send(*design, press(Action::confirm), 0.3f);
    EXPECT_GT(settings_.ui_volume, 0);
    send(*design, press(Action::confirm), 0.3f);
    EXPECT_EQ(settings_.ui_volume, 0);
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
    EXPECT_EQ(count_shelves(*design), 2); // the two genres
    expect_drawable(*design);
}

TEST_F(MediathekTest, GenresOnlyStillHasAShelfWhenNoFilmHasAGenre)
{
    fill_library();
    add_second_shelf();
    settings_.home_shelves = 2;
    auto design = make();
    EXPECT_EQ(count_shelves(*design), 1);
    EXPECT_GE(play_pills(*design), 1); // a film is focused, the billboard has its Play
    send(*design, press(Action::confirm), 0.4f);
    expect_drawable(*design);
}

TEST_F(MediathekTest, TheHomeShelvesRowChangesHomeAtOnce)
{
    fill_library_with_genres();
    auto design = make();
    open_category(*design, 1); // Appearance
    send(*design, nav(Direction::down), 0.3f);
    send(*design, nav(Direction::down), 0.3f);
    context_.settings_changed = false;
    send(*design, nav(Direction::right), 0.3f); // All -> own collections
    EXPECT_EQ(settings_.home_shelves, 1);
    EXPECT_TRUE(context_.settings_changed);
    send(*design, press(Action::confirm), 0.3f); // -> genres
    EXPECT_EQ(settings_.home_shelves, 2);
    send(*design, press(Action::back), 0.3f);
    send(*design, press(Action::back), 0.5f); // Home
    EXPECT_EQ(count_shelves(*design), 2);
}

TEST_F(MediathekTest, HoldingRightIntoTheFormDoesNotChangeTheLanguage)
{
    auto design = make();
    open_category(*design, 2); // Language; the press that opened it is still held
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

TEST_F(MediathekTest, TheDeveloperFormSkipsItsReadOnlyVersionRow)
{
    auto design = make();
    open_category(*design, 4);
    expect_drawable(*design);
    send(*design, nav(Direction::down), 0.3f); // nothing focusable below Debug info
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
    open_category(*design, 2); // Language
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
    open_category(*design, 0);
    send(*design, press(Action::confirm), 0.4f); // flips "off" to on, not on to off
    EXPECT_EQ(settings_.ui_volume, hui::Settings{}.ui_volume);
    send(*design, press(Action::back), 0.3f);
    send(*design, nav(Direction::down), 0.3f); // Appearance
    send(*design, nav(Direction::right), 0.3f);
    send(*design, press(Action::confirm), 0.4f);
    EXPECT_FALSE(settings_.pin_billboard);
    send(*design, press(Action::back), 0.3f);
    send(*design, nav(Direction::down), 0.3f); // Language
    send(*design, nav(Direction::down), 0.3f); // Library
    send(*design, nav(Direction::down), 0.3f); // Developer
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
    open_category(*design, 3); // Library
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
    open_category(*design, 3); // Library
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
    open_category(*design, 3); // Library
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
    open_category(*design, 3); // Library
    send(*design, press(Action::confirm), 0.6f);
    EXPECT_TRUE(played(audio::Cue::error));
    EXPECT_EQ(asked, 0);
}

} // namespace
