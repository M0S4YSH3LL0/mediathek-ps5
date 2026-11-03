// Mediathek - Tests for the first-seen store: Recently Added follows the library, not file times.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/added.hpp"
#include "media/library.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

using namespace hui::media;

constexpr std::int64_t kToday = 1800000000;

FoundFile video(const std::string &path, std::int64_t mtime, std::uint64_t size = 1000)
{
    FoundFile file;
    file.path = path;
    file.readable = path;
    file.size = size;
    file.mtime = mtime;
    return file;
}

ScanResult scan_of(std::vector<FoundFile> videos)
{
    ScanResult scan;
    scan.roots = {"/data/movies", "/data/tvshows"};
    scan.videos = std::move(videos);
    return scan;
}

const Movie &titled(const Library &library, const std::string &title)
{
    for (const Movie &movie : library.movies())
        if (movie.title == title)
            return movie;
    static const Movie none;
    ADD_FAILURE() << "no " << title;
    return none;
}

TEST(AddedStore, TheFirstStartKeepsTheFilesOwnTimesButNoneInTheFuture)
{
    Library library = Library::from_scan(scan_of({
        video("/data/movies/Blue Harbor (2011).mkv", 1500000000),
        video("/data/movies/Night Garden (2019).mkv", kToday + 5000), // a clock ahead
    }));
    AddedStore store;
    EXPECT_TRUE(store.apply(&library, kToday));
    EXPECT_EQ(titled(library, "Blue Harbor").added, 1500000000);
    EXPECT_EQ(titled(library, "Night Garden").added, kToday);
    EXPECT_EQ(store.size(), 2u);
}

TEST(AddedStore, AFileWithAnOldTimeCopiedLaterIsNew)
{
    AddedStore store;
    Library first =
        Library::from_scan(scan_of({video("/data/movies/Blue Harbor (2011).mkv", 1500000000)}));
    store.apply(&first, kToday);

    // Copied a week later, keeping the time it was made ten years ago.
    Library second = Library::from_scan(scan_of({
        video("/data/movies/Blue Harbor (2011).mkv", 1500000000),
        video("/data/movies/Old Lantern (1998).mkv", 1400000000),
    }));
    EXPECT_TRUE(store.apply(&second, kToday + 604800));
    EXPECT_EQ(titled(second, "Old Lantern").added, kToday + 604800);
    EXPECT_EQ(titled(second, "Blue Harbor").added, 1500000000);

    // Seen again: nothing changes, nothing to save.
    Library third = Library::from_scan(scan_of({
        video("/data/movies/Blue Harbor (2011).mkv", 1500000000),
        video("/data/movies/Old Lantern (1998).mkv", 1400000000),
    }));
    EXPECT_FALSE(store.apply(&third, kToday + 999999));
    EXPECT_EQ(titled(third, "Old Lantern").added, kToday + 604800);
}

TEST(AddedStore, AFileMovedToAnotherDriveKeepsItsDate)
{
    AddedStore store;
    Library first =
        Library::from_scan(scan_of({video("/data/movies/Blue Harbor (2011).mkv", 1500000000)}));
    store.apply(&first, kToday);
    ScanResult moved = scan_of({video("/mnt/usb0/movies/Blue Harbor (2011).mkv", kToday + 50)});
    moved.roots = {"/mnt/usb0/movies"};
    Library second = Library::from_scan(moved);
    EXPECT_FALSE(store.apply(&second, kToday + 100));
    EXPECT_EQ(titled(second, "Blue Harbor").added, 1500000000);
}

TEST(AddedStore, ASeriesIsAsNewAsItsNewestEpisode)
{
    AddedStore store;
    Library first = Library::from_scan(scan_of({
        video("/data/tvshows/Tidewater County/Season 1/Tidewater.County.S01E01.mkv", 1500000000),
    }));
    store.apply(&first, kToday);
    EXPECT_EQ(titled(first, "Tidewater County").added, 1500000000);

    Library second = Library::from_scan(scan_of({
        video("/data/tvshows/Tidewater County/Season 1/Tidewater.County.S01E01.mkv", 1500000000),
        video("/data/tvshows/Tidewater County/Season 1/Tidewater.County.S01E02.mkv", 1400000000),
    }));
    EXPECT_TRUE(store.apply(&second, kToday + 3600));
    const Movie &show = titled(second, "Tidewater County");
    EXPECT_EQ(show.added, kToday + 3600);
    ASSERT_EQ(show.seasons.size(), 1u);
    ASSERT_EQ(show.seasons[0].episodes.size(), 2u);
    EXPECT_EQ(show.seasons[0].episodes[0].added, 1500000000);
    EXPECT_EQ(show.seasons[0].episodes[1].added, kToday + 3600);
}

TEST(AddedStore, SurvivesARestart)
{
    AddedStore store;
    Library first =
        Library::from_scan(scan_of({video("/data/movies/Blue Harbor (2011).mkv", 1500000000)}));
    store.apply(&first, kToday);

    AddedStore restarted;
    ASSERT_TRUE(restarted.decode(store.encode()));
    EXPECT_EQ(restarted.size(), 1u);
    Library second = Library::from_scan(scan_of({
        video("/data/movies/Blue Harbor (2011).mkv", 1500000000),
        video("/data/movies/Old Lantern (1998).mkv", 1400000000),
    }));
    EXPECT_TRUE(restarted.apply(&second, kToday + 10));
    EXPECT_EQ(titled(second, "Blue Harbor").added, 1500000000);
    EXPECT_EQ(titled(second, "Old Lantern").added, kToday + 10); // a record exists: it is new
}

TEST(AddedStore, AnEmptyRecordStillMeansNewFilesAreNew)
{
    // The first start found no films; the films copied since are new, not as old as their files.
    AddedStore store;
    Library empty;
    EXPECT_TRUE(store.apply(&empty, kToday));
    AddedStore restarted;
    ASSERT_TRUE(restarted.decode(store.encode()));
    Library later =
        Library::from_scan(scan_of({video("/data/movies/Old Lantern (1998).mkv", 1400000000)}));
    restarted.apply(&later, kToday + 10);
    EXPECT_EQ(titled(later, "Old Lantern").added, kToday + 10);
}

TEST(AddedStore, RefusesOtherTextAndSkipsDamagedLines)
{
    AddedStore store;
    EXPECT_FALSE(store.decode("something else\nabc 12\n"));
    EXPECT_EQ(store.size(), 0u);
    ASSERT_TRUE(store.decode("mediathek-added 1\nabc 12\n\nbroken\n 5\ndef x\nghi 34"));
    EXPECT_EQ(store.size(), 2u);
    EXPECT_EQ(store.encode(), "mediathek-added 1\nabc 12\nghi 34\n");
}

} // namespace
