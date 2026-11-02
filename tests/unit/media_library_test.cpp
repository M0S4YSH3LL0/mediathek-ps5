// Mediathek - Tests for file-name parsing, the scan index, artwork pairing and resume points.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/library.hpp"
#include "media/progress.hpp"
#include "media/title.hpp"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using namespace hui::media;

TEST(MovieTitle, ReadsTheClassicNameWithYearInBrackets)
{
    const ParsedName name = parse_file_name("The Long Walk Home (2014).mkv");
    EXPECT_EQ(name.title, "The Long Walk Home");
    EXPECT_EQ(name.year, 2014);
    EXPECT_EQ(name.quality, "");
}

TEST(MovieTitle, ReadsASceneNameAndKeepsTheQuality)
{
    const ParsedName name = parse_file_name("the.long.walk.home.2014.1080p.BluRay.x264-GRP.mkv");
    EXPECT_EQ(name.title, "The Long Walk Home");
    EXPECT_EQ(name.year, 2014);
    EXPECT_EQ(name.quality, "1080p");
}

TEST(MovieTitle, AYearAsTheFirstWordIsTheTitle)
{
    const ParsedName name = parse_file_name("1917 (2019) 2160p.mp4");
    EXPECT_EQ(name.title, "1917");
    EXPECT_EQ(name.year, 2019);
    EXPECT_EQ(name.quality, "4K");
}

TEST(MovieTitle, ATitleThatContainsAYearKeepsIt)
{
    const ParsedName name = parse_file_name("2001 A Space Odyssey 1968.mkv");
    EXPECT_EQ(name.title, "2001 A Space Odyssey");
    EXPECT_EQ(name.year, 1968);
}

TEST(MovieTitle, UnderscoresAndBracketsAreSeparators)
{
    const ParsedName name = parse_file_name("night_train_[1987]_[4K].mp4");
    EXPECT_EQ(name.title, "Night Train");
    EXPECT_EQ(name.year, 1987);
    EXPECT_EQ(name.quality, "4K");
}

TEST(MovieTitle, ANameWithoutAnythingKnownStaysAsItIs)
{
    const ParsedName name = parse_file_name("Holiday Clips.avi");
    EXPECT_EQ(name.title, "Holiday Clips");
    EXPECT_EQ(name.year, 0);
}

TEST(MovieTitle, RecognisesVideoImageAndSubtitleFiles)
{
    EXPECT_TRUE(is_video_file("a.MKV"));
    EXPECT_TRUE(is_video_file("a.m2ts"));
    EXPECT_FALSE(is_video_file("a.jpg"));
    EXPECT_FALSE(is_video_file("noextension"));
    EXPECT_TRUE(is_image_file("poster.PNG"));
    EXPECT_TRUE(is_subtitle_file("a.en.srt"));
}

TEST(MovieTitle, SortKeyIgnoresArticlesAndPunctuation)
{
    EXPECT_EQ(sort_key("The Matrix"), "matrix");
    EXPECT_EQ(sort_key("A Quiet Place"), "quiet place");
    EXPECT_EQ(sort_key("Amélie"), "amlie");
    EXPECT_LT(sort_key("The Apartment"), sort_key("Brazil"));
}

constexpr const char *kIndex =
    "mediathek-index 1\n"
    "R\t/data/movies\n"
    "R\t/mnt/usb0/movies\n"
    "V\t/data/movies/Blue Harbor (2011).mkv\t4000000000\t1700000000\n"
    "I\t/data/movies/Blue Harbor (2011)-poster.jpg\t/data/mediathek/art/aa.jpg\n"
    "I\t/data/movies/Blue Harbor (2011)-fanart.jpg\t/data/mediathek/art/bb.jpg\n"
    "V\t/mnt/usb0/movies/Space Sagas/Saga One.mkv\t3000\t1600000000\n"
    "V\t/mnt/usb0/movies/Space Sagas/Saga Two.mkv\t3100\t1600000100\n"
    "S\t/data/movies/Blue Harbor (2011).en.srt\t/data/mediathek/art/cc.srt\n"
    "V\t/mnt/usb0/movies/Solo Film (2020)/video.mp4\t99\t1650000000\n"
    "I\t/mnt/usb0/movies/Solo Film (2020)/poster.jpg\n"
    "end\n";

TEST(MovieIndex, ParsesRootsAndFiles)
{
    ScanResult scan;
    ASSERT_TRUE(parse_index(kIndex, &scan));
    EXPECT_EQ(scan.roots.size(), 2u);
    EXPECT_EQ(scan.videos.size(), 4u);
    EXPECT_EQ(scan.images.size(), 3u);
    EXPECT_EQ(scan.subtitles.size(), 1u);
    EXPECT_EQ(scan.videos[0].size, 4000000000ull);
    EXPECT_EQ(scan.images[0].readable, "/data/mediathek/art/aa.jpg");
    EXPECT_EQ(scan.images[2].readable, scan.images[2].path); // no cache copy: same file
}

TEST(MovieIndex, RefusesAnIndexThatWasNotFinished)
{
    ScanResult scan;
    std::string text = kIndex;
    text.resize(text.size() - 4); // loses "end\n"
    EXPECT_FALSE(parse_index(text, &scan));
    EXPECT_FALSE(parse_index("something else\nend\n", &scan));
    EXPECT_FALSE(parse_index("", &scan));
}

TEST(MovieLibrary, PairsArtworkAndSubtitlesWithTheirMovie)
{
    ScanResult scan;
    ASSERT_TRUE(parse_index(kIndex, &scan));
    const Library library = Library::from_scan(scan);
    ASSERT_EQ(library.size(), 4u);

    const int blue = [&]
    {
        for (std::size_t i = 0; i < library.size(); ++i)
            if (library[i].title == "Blue Harbor")
                return static_cast<int>(i);
        return -1;
    }();
    ASSERT_GE(blue, 0);
    const Movie &movie = library[static_cast<std::size_t>(blue)];
    EXPECT_EQ(movie.year, 2011);
    EXPECT_EQ(movie.poster, "/data/mediathek/art/aa.jpg");
    EXPECT_EQ(movie.backdrop, "/data/mediathek/art/bb.jpg");
    ASSERT_EQ(movie.subtitles.size(), 1u);
    EXPECT_EQ(movie.subtitles[0].file, "/data/mediathek/art/cc.srt");
    EXPECT_EQ(movie.drive, "Internal");
    EXPECT_EQ(movie.collection, "");
}

TEST(MovieLibrary, ASoleFilmFolderGivesItsNameAndPosterButNoCollection)
{
    ScanResult scan;
    ASSERT_TRUE(parse_index(kIndex, &scan));
    const Library library = Library::from_scan(scan);
    for (const Movie &movie : library.movies())
    {
        if (movie.path.find("Solo Film") == std::string::npos)
            continue;
        EXPECT_EQ(movie.title, "Solo Film");
        EXPECT_EQ(movie.year, 2020);
        EXPECT_EQ(movie.poster, "/mnt/usb0/movies/Solo Film (2020)/poster.jpg");
        EXPECT_EQ(movie.collection, "");
        EXPECT_EQ(movie.drive, "USB 1");
        return;
    }
    FAIL() << "Solo Film missing";
}

TEST(MovieLibrary, ASharedFolderIsACollection)
{
    ScanResult scan;
    ASSERT_TRUE(parse_index(kIndex, &scan));
    const Library library = Library::from_scan(scan);
    int in_collection = 0;
    for (const Movie &movie : library.movies())
        if (movie.collection == "Space Sagas")
            ++in_collection;
    EXPECT_EQ(in_collection, 2);
}

TEST(MovieLibrary, ListsTheSameFileOnceAndSortsByTitle)
{
    ScanResult scan;
    ASSERT_TRUE(parse_index(kIndex, &scan));
    scan.videos.push_back(scan.videos[0]); // reachable through two roots
    const Library library = Library::from_scan(scan);
    ASSERT_EQ(library.size(), 4u);
    EXPECT_EQ(library[0].title, "Blue Harbor");
    EXPECT_EQ(library[1].title, "Saga One");
    EXPECT_GE(library.find(library[2].id), 0);
    EXPECT_EQ(library.find("nope"), -1);
}

// A folder per film with its subtitles in "subs", and the same movies folder
// reachable under a second name (a drive that ignores letter case does this).
TEST(MovieLibrary, ScansAFolderOnceWhateverItIsCalled)
{
    char pattern[] = "/tmp/mediathek-walk-XXXXXX";
    ASSERT_NE(mkdtemp(pattern), nullptr);
    const std::string base = pattern;
    const std::string movies = base + "/movies";
    for (const char *film : {"Aa (2001)", "Bb (2002)", "Cc (2003)"})
    {
        const std::string folder = movies + "/" + film;
        ASSERT_EQ(system(("mkdir -p '" + folder + "/subs'").c_str()), 0);
        std::ofstream(folder + "/" + film + ".mkv") << "x";
        std::ofstream(folder + "/subs/" + film + ".en.srt") << "x";
    }
    ASSERT_EQ(symlink(movies.c_str(), (base + "/Movies").c_str()), 0);

    const ScanResult scan = walk_folders({movies, base + "/Movies"});
    EXPECT_EQ(scan.roots.size(), 1u);
    EXPECT_EQ(scan.videos.size(), 3u);
    EXPECT_EQ(system(("rm -rf '" + base + "'").c_str()), 0);
}

TEST(MovieLibrary, FindsSubtitlesInASubsFolder)
{
    ScanResult scan;
    scan.roots = {"/data/movies"};
    const auto file = [](const char *path, const char *readable, std::uint64_t size)
    {
        FoundFile found;
        found.path = path;
        found.readable = readable;
        found.size = size;
        return found;
    };
    scan.videos = {file("/data/movies/Aa (2001)/Aa (2001).mkv", "", 10),
                   file("/data/movies/Bb (2002)/Bb (2002).mkv", "", 11),
                   file("/data/movies/Shared/One.mkv", "", 12),
                   file("/data/movies/Shared/Two.mkv", "", 13)};
    scan.subtitles = {file("/data/movies/Aa (2001)/Subs/2_French.srt", "/art/fr.srt", 0),
                      file("/data/movies/Aa (2001)/Subs/3_English.srt", "/art/en.srt", 0),
                      file("/data/movies/Bb (2002)/subs/Bb (2002).de.srt", "/art/de.srt", 0),
                      file("/data/movies/Shared/subs/One.srt", "/art/one.srt", 0),
                      file("/data/movies/Shared/subs/2_English.srt", "/art/other.srt", 0)};
    const Library library = Library::from_scan(scan);
    ASSERT_EQ(library.size(), 4u);
    for (std::size_t i = 0; i < library.size(); ++i)
    {
        const Movie &movie = library[i];
        // The language comes from the original file's name: the copy's name ("/art/en.srt" here,
        // a hash on a console) says nothing.
        if (movie.title == "Aa")
        {
            // alone: owns the folder, English first, then the rest in the order found
            ASSERT_EQ(movie.subtitles.size(), 2u);
            EXPECT_EQ(movie.subtitles[0].file, "/art/en.srt");
            EXPECT_EQ(movie.subtitles[0].language, "eng");
            EXPECT_EQ(movie.subtitles[1].file, "/art/fr.srt");
            EXPECT_EQ(movie.subtitles[1].language, "fre");
        }
        else if (movie.title == "Bb")
        {
            ASSERT_EQ(movie.subtitles.size(), 1u);
            EXPECT_EQ(movie.subtitles[0].file, "/art/de.srt");
            EXPECT_EQ(movie.subtitles[0].language, "ger");
        }
        else if (movie.title == "One")
        {
            ASSERT_EQ(movie.subtitles.size(), 1u); // shared: only its own name
            EXPECT_EQ(movie.subtitles[0].file, "/art/one.srt");
            EXPECT_EQ(movie.subtitles[0].language, "und"); // "One.srt" names no language
        }
        else
        {
            EXPECT_TRUE(movie.subtitles.empty()); // "Two" has none
        }
    }
}

TEST(MovieLibrary, SearchesMoviesAndMediathekMoviesOnEveryDrive)
{
    const std::vector<std::string> roots = default_roots();
    const auto has = [&](const std::string &root)
    { return std::find(roots.begin(), roots.end(), root) != roots.end(); };
    for (const std::string drive :
         {"/data", "", "/mnt/usb0", "/mnt/usb7", "/mnt/ext0", "/mnt/ext1"})
    {
        EXPECT_TRUE(has(drive + "/movies")) << drive;
        EXPECT_TRUE(has(drive + "/Movies")) << drive;
        EXPECT_TRUE(has(drive + "/mediathek/movies")) << drive;
        EXPECT_TRUE(has(drive + "/mediathek/Movies")) << drive;
    }
    EXPECT_EQ(roots.size(), 48u); // helper/mt-scan.c keeps at most 64
}

// A collection folder is the first one under the root, also when the root is
// mediathek/movies, and the drive is still named from the path.
TEST(MovieLibrary, AMediathekMoviesFolderWorksLikeAMoviesFolder)
{
    ScanResult scan;
    scan.roots = {"/mnt/usb0/movies", "/mnt/usb0/mediathek/movies"};
    for (const char *path : {"/mnt/usb0/mediathek/movies/Space Sagas/Part One (2001).mkv",
                             "/mnt/usb0/mediathek/movies/Space Sagas/Part Two (2003).mkv",
                             "/mnt/usb0/movies/Blue Harbor (2011).mkv"})
    {
        FoundFile file;
        file.path = path;
        file.readable = path;
        scan.videos.push_back(file);
    }
    const Library library = Library::from_scan(scan);
    ASSERT_EQ(library.size(), 3u);
    int in_collection = 0;
    for (const Movie &movie : library.movies())
        if (movie.collection == "Space Sagas")
            ++in_collection;
    EXPECT_EQ(in_collection, 2);
    EXPECT_EQ(drive_label("/mnt/usb0/mediathek/movies/a.mkv"), "USB 1");
    EXPECT_EQ(drive_label("/data/mediathek/movies/a.mkv"), "Internal");
}

TEST(MovieLibrary, NamesTheDrives)
{
    EXPECT_EQ(drive_label("/data/movies/a.mkv"), "Internal");
    EXPECT_EQ(drive_label("/mnt/usb0/movies/a.mkv"), "USB 1");
    EXPECT_EQ(drive_label("/mnt/usb3/movies/a.mkv"), "USB 4");
    EXPECT_EQ(drive_label("/mnt/ext1/movies/a.mkv"), "Drive 2");
}

TEST(MovieProgress, ResumesInTheMiddleAndForgetsTheCredits)
{
    ProgressStore store;
    EXPECT_FALSE(store.in_progress("a"));
    store.update("a", 5.0, 6000.0, 100); // too early to matter
    EXPECT_FALSE(store.in_progress("a"));
    store.update("a", 1500.0, 6000.0, 200);
    EXPECT_TRUE(store.in_progress("a"));
    EXPECT_DOUBLE_EQ(store.resume_position("a"), 1500.0);
    EXPECT_FLOAT_EQ(store.fraction("a"), 0.25f);
    store.update("a", 5600.0, 6000.0, 300); // 93 %: counts as watched
    EXPECT_TRUE(store.watched("a"));
    EXPECT_FALSE(store.in_progress("a"));
    EXPECT_DOUBLE_EQ(store.resume_position("a"), 0.0);
    store.update("a", 900.0, 6000.0, 400); // watched again from the start
    EXPECT_FALSE(store.watched("a"));
}

TEST(MovieProgress, ListsWhatIsInProgressNewestFirst)
{
    ProgressStore store;
    store.update("old", 600.0, 6000.0, 100);
    store.update("new", 600.0, 6000.0, 300);
    store.update("done", 5990.0, 6000.0, 200);
    const auto list = store.in_progress_recent();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0]->id, "new");
    EXPECT_EQ(list[1]->id, "old");
}

TEST(MovieProgress, SurvivesASaveAndRefusesAGarbledOne)
{
    ProgressStore store;
    store.update("aaaa1111", 1234.5, 7000.0, 1700000000);
    store.set_watched("bbbb2222", true, 1700000100);
    ProgressStore loaded;
    ASSERT_TRUE(loaded.decode(store.encode()));
    EXPECT_EQ(loaded.size(), 2u);
    EXPECT_NEAR(loaded.resume_position("aaaa1111"), 1234.5, 0.05);
    EXPECT_TRUE(loaded.watched("bbbb2222"));
    EXPECT_FALSE(loaded.dirty());
    EXPECT_FALSE(loaded.decode("garbage line\n"));
    EXPECT_EQ(loaded.size(), 0u);
}

TEST(MovieProgress, MarkingWatchedClearsTheResumePoint)
{
    ProgressStore store;
    store.update("a", 2000.0, 6000.0, 1);
    store.set_watched("a", true, 2);
    EXPECT_FALSE(store.in_progress("a"));
    store.set_watched("a", false, 3);
    EXPECT_FALSE(store.watched("a"));
    store.forget("a");
    EXPECT_EQ(store.find("a"), nullptr);
}

} // namespace

TEST(MediaSearch, FoldsCaseAccentsAndPunctuation)
{
    EXPECT_EQ(search_fold("Am\xC3\xA9lie"), "amelie");
    EXPECT_EQ(search_fold("\xC3\x9C"
                          "ber den D\xC3\xA4"
                          "chern"),
              "uber den dachern");
    EXPECT_EQ(search_fold("Stra\xC3\x9F"
                          "e"),
              "strasse");
    EXPECT_EQ(search_fold("  Spider-Man: Home!  "), "spider man home");
    EXPECT_EQ(search_fold(""), "");
}

TEST(MediaSearch, EveryWordMustMatchTheTitleAndNothingElse)
{
    Movie movie;
    movie.title = "Blue Harbor";
    movie.year = 2011;
    movie.collection = "Sea Stories";
    movie.genres = {35}; // Comedy
    EXPECT_TRUE(matches_search(movie, ""));
    EXPECT_TRUE(matches_search(movie, "harb"));
    EXPECT_TRUE(matches_search(movie, "BLUE harbor"));
    EXPECT_TRUE(matches_search(movie, "harbor blue"));
    EXPECT_FALSE(matches_search(movie, "blue red"));
    EXPECT_FALSE(matches_search(movie, "harbour"));
    // The year, the folder and the genres are not searched: a title does not turn up for them.
    EXPECT_FALSE(matches_search(movie, "2011"));
    EXPECT_FALSE(matches_search(movie, "sea"));
    EXPECT_FALSE(matches_search(movie, "comedy"));
}

TEST(MediaGenreShelves, AFilmIsOnTheShelvesOfItsFirstTwoGenresAndNoOther)
{
    // Genre numbers: 18 Drama, 35 Comedy, 878 Science Fiction, 27 Horror, 28 Action.
    const std::vector<std::vector<int>> genres = {{18, 878, 28}, {878, 18}, {35, 18},
                                                  {27},          {},        {999999, 35, 27, 18}};
    std::vector<Movie> movies(genres.size());
    for (std::size_t i = 0; i < movies.size(); ++i)
        movies[i].genres = genres[i];

    EXPECT_EQ(main_genres(movies[0]), (std::vector<int>{18, 878}));
    EXPECT_TRUE(main_genres(movies[4]).empty());
    EXPECT_EQ(main_genres(movies[5]), (std::vector<int>{35, 27})); // the unnamed one is skipped

    const std::vector<GenreShelf> shelves = genre_shelves(movies);
    // Every genre that is a main one of some film has a shelf, even with a single film; Action
    // is only a third genre and has none.
    ASSERT_EQ(shelves.size(), 4u);
    EXPECT_EQ(shelves[0].genre, 18);
    EXPECT_EQ(shelves[0].movies, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(shelves[1].genre, 27); // Horror and Comedy and Sci-Fi: two films each ...
    EXPECT_EQ(shelves[1].movies, (std::vector<int>{3, 5}));
    EXPECT_EQ(shelves[2].genre, 35); // ... the lower genre number first
    EXPECT_EQ(shelves[2].movies, (std::vector<int>{2, 5}));
    EXPECT_EQ(shelves[3].genre, 878);
    EXPECT_EQ(shelves[3].movies, (std::vector<int>{0, 1}));
    EXPECT_TRUE(genre_shelves({}).empty());
}
