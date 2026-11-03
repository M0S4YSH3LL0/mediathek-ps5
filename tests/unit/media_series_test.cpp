// Mediathek - Tests for series: episode names, grouping files into series, the episode up next,
// TMDB. Copyright (C) 2026 BlackBearReloaded SPDX-License-Identifier: GPL-3.0-or-later

#include "media/library.hpp"
#include "media/progress.hpp"
#include "media/show_progress.hpp"
#include "media/title.hpp"
#include "media/tmdb.hpp"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

namespace
{

using namespace hui::media;

TEST(EpisodeName, ReadsTheSceneMarkWithShowAndTitle)
{
    const EpisodeName name =
        parse_episode_name("Show.Name.S01E02.The.Pilot.1080p.WEB.x264-GRP.mkv");
    EXPECT_TRUE(name.marked);
    EXPECT_EQ(name.season, 1);
    EXPECT_EQ(name.episode, 2);
    EXPECT_EQ(name.show, "Show Name");
    EXPECT_EQ(name.title, "The Pilot");
}

TEST(EpisodeName, ReadsTheOtherSpellingsOfTheMark)
{
    const EpisodeName cross = parse_episode_name("Show Name - 1x02 - The Pilot.mkv");
    EXPECT_TRUE(cross.marked);
    EXPECT_EQ(cross.season, 1);
    EXPECT_EQ(cross.episode, 2);
    EXPECT_EQ(cross.show, "Show Name");
    EXPECT_EQ(cross.title, "The Pilot");

    const EpisodeName apart = parse_episode_name("Show Name S01 E02.mkv");
    EXPECT_TRUE(apart.marked);
    EXPECT_EQ(apart.season, 1);
    EXPECT_EQ(apart.episode, 2);
    EXPECT_EQ(apart.title, "");

    const EpisodeName bare = parse_episode_name("s2e13.mp4");
    EXPECT_TRUE(bare.marked);
    EXPECT_EQ(bare.season, 2);
    EXPECT_EQ(bare.episode, 13);
    EXPECT_EQ(bare.show, "");
}

TEST(EpisodeName, ANumberAloneCountsTheEpisodeButIsNoMark)
{
    const EpisodeName folge = parse_episode_name("Folge 5 - Der Anfang.mkv");
    EXPECT_FALSE(folge.marked);
    EXPECT_EQ(folge.season, -1);
    EXPECT_EQ(folge.episode, 5);
    EXPECT_EQ(folge.title, "Der Anfang");

    const EpisodeName leading = parse_episode_name("05 - Low Water.mkv");
    EXPECT_FALSE(leading.marked);
    EXPECT_EQ(leading.episode, 5);
    EXPECT_EQ(leading.title, "Low Water");
}

TEST(EpisodeName, AFilmNameIsNoEpisode)
{
    for (const char *film : {"Blue Harbor (2011).mkv", "The.Last.Lighthouse.2019.2160p.mkv",
                             "Sample.1920x1080.mkv", "2001 Odyssey (1968).mkv"})
    {
        const EpisodeName name = parse_episode_name(film);
        EXPECT_FALSE(name.marked) << film;
        EXPECT_EQ(name.season, -1) << film;
    }
}

TEST(EpisodeName, ReadsTheSeasonOfAFolder)
{
    EXPECT_EQ(season_of_folder("Season 1"), 1);
    EXPECT_EQ(season_of_folder("Season1"), 1);
    EXPECT_EQ(season_of_folder("season 01"), 1);
    EXPECT_EQ(season_of_folder("Season.12"), 12);
    EXPECT_EQ(season_of_folder("Staffel 2"), 2);
    EXPECT_EQ(season_of_folder("S03"), 3);
    EXPECT_EQ(season_of_folder("Specials"), 0);
    EXPECT_EQ(season_of_folder("Tidewater County"), -1);
    EXPECT_EQ(season_of_folder("Seasons"), -1);
    EXPECT_EQ(season_of_folder("S"), -1);
}

TEST(EpisodeName, KnowsTheSeriesFolders)
{
    for (const char *name : {"tvshows", "TVShows", "TV Shows", "series", "Series", "Serien"})
        EXPECT_TRUE(is_series_folder(name)) << name;
    for (const char *name : {"movies", "Movies", "Series 1", "films"})
        EXPECT_FALSE(is_series_folder(name)) << name;
}

FoundFile video(const std::string &path, std::uint64_t size = 1000)
{
    FoundFile file;
    file.path = path;
    file.readable = path;
    file.size = size;
    file.mtime = 1700000000;
    return file;
}

const Movie *find_titled(const Library &library, const std::string &title)
{
    for (const Movie &movie : library.movies())
        if (movie.title == title)
            return &movie;
    return nullptr;
}

ScanResult series_scan()
{
    ScanResult scan;
    scan.roots = {"/data/tvshows", "/data/movies"};
    scan.videos = {
        video("/data/tvshows/Tidewater County/Season 1/Tidewater.County.S01E02.Low.Water.mkv"),
        video("/data/tvshows/Tidewater County/Season 1/Tidewater.County.S01E01.The.Ferry.mkv"),
        // No mark: the season comes from the folder, the number from the name.
        video("/data/tvshows/Tidewater County/season2/Episode 1.mkv"),
        video("/data/tvshows/Tidewater County/Specials/Tidewater.County.S00E01.mkv"),
        video("/data/movies/Blue Harbor (2011).mkv"),
        // Marked, in a movies folder: a series of its folder's name.
        video("/data/movies/Other Show/Other.Show.S01E01.mkv"),
        // Marked, with no folder: a series of the name before the mark.
        video("/data/movies/Lone.Show.S02E03.mkv"),
        // Marked but nameless, straight in a movies folder: nothing to file it under.
        video("/data/movies/S01E01.mkv"),
    };
    FoundFile poster;
    poster.path = "/data/tvshows/Tidewater County/poster.jpg";
    poster.readable = "/art/tide-poster.jpg";
    FoundFile fanart;
    fanart.path = "/data/tvshows/Tidewater County/fanart.jpg";
    fanart.readable = "/art/tide-fanart.jpg";
    scan.images = {poster, fanart};
    return scan;
}

TEST(SeriesLibrary, GroupsEpisodesIntoSeriesAndSeasons)
{
    const Library library = Library::from_scan(series_scan());
    EXPECT_EQ(library.film_count(), 2u); // Blue Harbor and the nameless S01E01
    EXPECT_EQ(library.show_count(), 3u);

    const Movie *tide = find_titled(library, "Tidewater County");
    ASSERT_NE(tide, nullptr);
    EXPECT_TRUE(tide->show);
    EXPECT_EQ(tide->id.substr(0, 2), "tv");
    EXPECT_EQ(tide->poster, "/art/tide-poster.jpg");
    EXPECT_EQ(tide->backdrop, "/art/tide-fanart.jpg");
    EXPECT_EQ(tide->path, "/data/tvshows/Tidewater County");
    EXPECT_EQ(tide->size, 4000u);
    // Numbered seasons in order, the specials last.
    ASSERT_EQ(tide->seasons.size(), 3u);
    EXPECT_EQ(tide->seasons[0].number, 1);
    EXPECT_EQ(tide->seasons[1].number, 2);
    EXPECT_EQ(tide->seasons[2].number, 0);
    ASSERT_EQ(tide->seasons[0].episodes.size(), 2u);
    EXPECT_EQ(tide->seasons[0].episodes[0].number, 1);
    EXPECT_EQ(tide->seasons[0].episodes[0].title, "The Ferry");
    EXPECT_EQ(tide->seasons[0].episodes[1].number, 2);
    ASSERT_EQ(tide->seasons[1].episodes.size(), 1u);
    EXPECT_EQ(tide->seasons[1].episodes[0].number, 1);
    EXPECT_EQ(episode_count(*tide), 4u);
    EXPECT_EQ(season_total(*tide), 2u); // the specials are no season

    EXPECT_NE(find_titled(library, "Other Show"), nullptr);
    const Movie *lone = find_titled(library, "Lone Show");
    ASSERT_NE(lone, nullptr);
    ASSERT_EQ(lone->seasons.size(), 1u);
    EXPECT_EQ(lone->seasons[0].number, 2);
}

TEST(SeriesLibrary, FindsAnEpisodeByItsId)
{
    const Library library = Library::from_scan(series_scan());
    const Movie *tide = find_titled(library, "Tidewater County");
    ASSERT_NE(tide, nullptr);
    const std::string id = tide->seasons[1].episodes[0].id;
    EpisodeRef ref;
    EXPECT_EQ(library.find_episode(id, &ref), library.find(tide->id));
    EXPECT_EQ(ref, (EpisodeRef{1, 0}));
    EXPECT_EQ(library.find_episode("nothing", &ref), -1);
}

TEST(SeriesLibrary, StepsThroughTheEpisodesAcrossSeasonsButNotIntoTheSpecials)
{
    const Library library = Library::from_scan(series_scan());
    const Movie *tide = find_titled(library, "Tidewater County");
    ASSERT_NE(tide, nullptr);
    EXPECT_EQ(first_episode(*tide), (EpisodeRef{0, 0}));
    EXPECT_EQ(next_episode(*tide, {0, 0}), (EpisodeRef{0, 1}));
    EXPECT_EQ(next_episode(*tide, {0, 1}), (EpisodeRef{1, 0})); // into season 2
    EXPECT_FALSE(next_episode(*tide, {1, 0}).valid());          // the specials wait
    EXPECT_EQ(episode_at(*tide, {9, 9}), nullptr);
}

Movie four_episodes()
{
    Movie show;
    show.id = "tv1";
    show.show = true;
    const char *ids[2][2] = {{"a", "b"}, {"c", "d"}};
    for (int s = 0; s < 2; ++s)
    {
        Season season;
        season.number = s + 1;
        for (int e = 0; e < 2; ++e)
        {
            Episode episode;
            episode.id = ids[s][e];
            episode.season = s + 1;
            episode.number = e + 1;
            season.episodes.push_back(episode);
        }
        show.seasons.push_back(season);
    }
    return show;
}

TEST(ShowProgress, NothingPlayedStartsAtTheFirstEpisode)
{
    const ProgressStore progress;
    const ShowProgress state = show_progress(four_episodes(), progress);
    EXPECT_EQ(state.next, (EpisodeRef{0, 0}));
    EXPECT_FALSE(state.continuing);
    EXPECT_FALSE(state.all_watched);
    EXPECT_EQ(state.last, 0);
}

TEST(ShowProgress, AnEpisodeUnderWayIsResumed)
{
    ProgressStore progress;
    progress.set_watched("a", true, 10);
    progress.update("b", 600.0, 2700.0, 20);
    const ShowProgress state = show_progress(four_episodes(), progress);
    EXPECT_EQ(state.next, (EpisodeRef{0, 1}));
    EXPECT_TRUE(state.continuing);
    EXPECT_EQ(state.last, 20);
}

TEST(ShowProgress, AfterAFinishedEpisodeComesTheNextUnseenOne)
{
    ProgressStore progress;
    progress.set_watched("a", true, 10);
    progress.set_watched("c", true, 5); // seen before, out of order
    progress.set_watched("b", true, 30);
    const ShowProgress state = show_progress(four_episodes(), progress);
    EXPECT_EQ(state.next, (EpisodeRef{1, 1})); // "c" is skipped
    EXPECT_TRUE(state.continuing);
}

TEST(ShowProgress, AFewSecondsLookedAtAreNoProgress)
{
    ProgressStore progress;
    progress.set_watched("a", true, 10);
    progress.update("c", 5.0, 2700.0, 50); // under the resume threshold
    const ShowProgress state = show_progress(four_episodes(), progress);
    EXPECT_EQ(state.next, (EpisodeRef{0, 1}));
    EXPECT_EQ(state.last, 10);
}

TEST(ShowProgress, EverythingSeenLeavesContinueWatching)
{
    ProgressStore progress;
    for (const char *id : {"a", "b", "c", "d"})
        progress.set_watched(id, true, 10);
    const ShowProgress state = show_progress(four_episodes(), progress);
    EXPECT_TRUE(state.all_watched);
    EXPECT_FALSE(state.continuing);
    EXPECT_EQ(state.next, (EpisodeRef{0, 0}));
}

// --- TMDB ---------------------------------------------------------------------------------

std::string temp_directory()
{
    char pattern[] = "/tmp/mediathek-series-XXXXXX";
    EXPECT_NE(mkdtemp(pattern), nullptr);
    return pattern;
}

std::string read_text(const std::string &path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

constexpr const char *kTvAnswer = R"({"page":1,"results":[
  {"id":4711,"name":"Tidewater County","first_air_date":"2015-03-01","poster_path":"/tide.jpg","backdrop_path":"/tide_bd.jpg","overview":"A ferry town.","vote_average":7.9,"vote_count":120,"genre_ids":[9648,10759,10765]}
]})";

constexpr const char *kSeasonAnswer =
    R"({"id":1,"name":"Season 1","overview":"The first winter.","season_number":1,"episodes":[
  {"episode_number":1,"name":"The Ferry","overview":"She misses the ferry."},
  {"episode_number":2,"name":"Low Water","overview":""}
]})";

TEST(SeriesTmdb, SearchesSeriesAndAsksForASeason)
{
    TmdbConfig config;
    config.key = "0123456789abcdef0123456789abcdef";
    const Request search = search_request(config, "Tidewater County", 2015, true);
    EXPECT_NE(search.url.find("/search/tv?query=Tidewater%20County"), std::string::npos);
    EXPECT_NE(search.url.find("&first_air_date_year=2015"), std::string::npos);
    const Request season = season_request(config, 4711, 2);
    EXPECT_NE(season.url.find("/tv/4711/season/2?language="), std::string::npos);
}

TEST(SeriesTmdb, ReadsASeriesHitAndTurnsItsGenresIntoFilmGenres)
{
    Match match;
    ASSERT_TRUE(pick_match(kTvAnswer, 2015, &match, true));
    EXPECT_EQ(match.id, 4711);
    EXPECT_EQ(match.title, "Tidewater County");
    // Mystery stays; "Action & Adventure" is both; "Sci-Fi & Fantasy" is both.
    EXPECT_EQ(match.genre_ids, (std::vector<int>{9648, 28, 12, 878, 14}));
    EXPECT_EQ(tv_genres_as_films({10763, 10768}), (std::vector<int>{10763, 10752}));
}

TEST(SeriesTmdb, ReadsASeason)
{
    SeasonAnswer season;
    ASSERT_TRUE(parse_season(kSeasonAnswer, &season));
    EXPECT_EQ(season.overview, "The first winter.");
    ASSERT_EQ(season.episodes.size(), 2u);
    EXPECT_EQ(season.episodes[0].number, 1);
    EXPECT_EQ(season.episodes[0].name, "The Ferry");
    EXPECT_FALSE(parse_season("not json", &season));
}

TEST(SeriesTmdb, FetchesTheSeriesThenItsSeasonsAndFillsThem)
{
    const Library library = Library::from_scan(series_scan());
    const Movie *tide = find_titled(library, "Tidewater County");
    ASSERT_NE(tide, nullptr);
    const CoverStore store(temp_directory());

    int searches = 0;
    std::vector<std::string> seasons;
    const HttpGet get =
        [&](const Request &request, std::size_t, std::string *body, std::string *error)
    {
        if (request.url.find("/search/tv") != std::string::npos)
        {
            ++searches;
            *body = kTvAnswer;
            return true;
        }
        if (const std::size_t at = request.url.find("/season/"); at != std::string::npos)
        {
            seasons.push_back(request.url.substr(at + 8, 1));
            if (request.url.find("/season/1?") == std::string::npos)
            {
                *error = "HTTP 404"; // TMDB lists no such season
                return false;
            }
            *body = kSeasonAnswer;
            return true;
        }
        *body = "\xFF\xD8\xFF\xE0 fake jpeg"; // a picture
        return true;
    };
    TmdbConfig config;
    config.key = "k";

    // The series has its own poster, so only its facts and seasons are asked for.
    const std::vector<CoverJob> jobs = missing_covers(library, store);
    const CoverJob *job = nullptr;
    for (const CoverJob &candidate : jobs)
        if (candidate.id == tide->id)
            job = &candidate;
    ASSERT_NE(job, nullptr);
    EXPECT_TRUE(job->show);
    EXPECT_FALSE(job->want_poster);
    EXPECT_EQ(job->seasons.size(), 3u);

    std::string note;
    EXPECT_EQ(fetch_cover(config, get, store, *job, &note), FetchOutcome::stored) << note;
    EXPECT_EQ(searches, 1);
    EXPECT_EQ(seasons, (std::vector<std::string>{"1", "2", "0"}));
    EXPECT_EQ(store.tmdb_id(tide->id), 4711);
    EXPECT_EQ(read_text(store.season_path(tide->id, 1)), "The first winter.");
    EXPECT_EQ(read_text(store.season_path(tide->id, 2)), "\n"); // not asked again

    Movie filled = *tide;
    store.fill(&filled);
    EXPECT_EQ(filled.seasons[0].overview, "The first winter.");
    EXPECT_EQ(filled.seasons[0].episodes[0].overview, "She misses the ferry.");
    EXPECT_EQ(filled.seasons[0].episodes[0].title, "The Ferry");
    EXPECT_EQ(filled.seasons[0].episodes[1].overview, "");
    // TMDB knows no season 2: the file name's title stays (here none).
    EXPECT_EQ(filled.seasons[1].episodes[0].title, "");

    // Everything is there now: the series is not asked again.
    for (const CoverJob &again : missing_covers(library, store))
        EXPECT_NE(again.id, tide->id);
}

TEST(SeriesTmdb, ANewSeasonIsFetchedWithoutSearchingAgain)
{
    const CoverStore store(temp_directory());
    ASSERT_TRUE(std::ofstream(store.tmdb_id_path("tv1")) << "4711");
    int searches = 0;
    int season_requests = 0;
    const HttpGet get = [&](const Request &request, std::size_t, std::string *body, std::string *)
    {
        if (request.url.find("/search/") != std::string::npos)
            ++searches;
        else
            ++season_requests;
        *body = kSeasonAnswer;
        return true;
    };
    CoverJob job;
    job.id = "tv1";
    job.title = "Tidewater County";
    job.show = true;
    job.want_poster = false;
    job.want_wallpaper = false;
    job.want_overview = false;
    job.want_facts = false;
    SeasonJob season;
    season.number = 1;
    season.episodes = {{1, "ep1"}};
    job.seasons = {season};
    TmdbConfig config;
    config.key = "k";
    std::string note;
    EXPECT_EQ(fetch_cover(config, get, store, job, &note), FetchOutcome::stored) << note;
    EXPECT_EQ(searches, 0);
    EXPECT_EQ(season_requests, 1);
    EXPECT_EQ(read_text(store.title_path("ep1")), "The Ferry");
}

} // namespace
