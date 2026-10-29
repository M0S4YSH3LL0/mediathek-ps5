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

} // namespace
