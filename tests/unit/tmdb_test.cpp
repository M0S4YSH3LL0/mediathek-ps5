// Mediathek - Tests for the JSON reader, the env file, TMDB matching and the cover service.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/json.hpp"
#include "media/cover_service.hpp"
#include "media/tmdb.hpp"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

namespace
{

using namespace hui::media;
namespace json = hui::json;

constexpr const char *kAnswer = R"({"page":1,"results":[
  {"title":"Dune","release_date":"1984-12-14","poster_path":"/old.jpg","backdrop_path":null},
  {"title":"Dune: Part Two","release_date":"2024-02-27","poster_path":"/two.jpg","backdrop_path":"/two_bd.jpg","overview":"  Paul unites with\n\tChani  and the Fremen.\r\n"},
  {"title":"No picture","release_date":"2024-01-01","poster_path":null,"backdrop_path":null}
],"total_results":3})";

std::string temp_directory()
{
    char pattern[] = "/tmp/mediathek-covers-XXXXXX";
    EXPECT_NE(mkdtemp(pattern), nullptr);
    return pattern;
}

// Plays TMDB: answers searches from a table and serves a tiny JPEG for any image.
struct FakeWeb
{
    std::atomic<int> searches{0};
    std::atomic<int> images{0};
    std::atomic<bool> offline{false};

    HttpGet get()
    {
        return [this](const Request &request, std::size_t, std::string *body, std::string *error)
        {
            if (offline)
            {
                *error = "Connection refused";
                return false;
            }
            if (request.url.find("/search/movie") != std::string::npos)
            {
                ++searches;
                const bool dune = request.url.find("query=Dune") != std::string::npos;
                *body = dune ? kAnswer : R"({"results":[]})";
                return true;
            }
            ++images;
            *body = std::string("\xFF\xD8\xFF\xE0 fake jpeg data ") + request.url;
            return true;
        };
    }
};

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

TEST(Json, ReadsObjectsArraysAndEscapes)
{
    json::Value value;
    ASSERT_TRUE(json::parse(R"({"a":[1,2.5,true,null],"s":"x\"yé🎬","n":{}})", &value));
    ASSERT_NE(value.get("a"), nullptr);
    EXPECT_EQ(value.get("a")->items.size(), 4u);
    EXPECT_EQ(value.get("a")->items[1].number, 2.5);
    EXPECT_EQ(value.text("s"), "x\"y\xC3\xA9\xF0\x9F\x8E\xAC");
    EXPECT_EQ(value.text("missing"), "");
    EXPECT_EQ(value.text("a"), ""); // not a string
}

TEST(Json, RefusesBrokenAndHostileText)
{
    json::Value value;
    EXPECT_FALSE(json::parse("", &value));
    EXPECT_FALSE(json::parse("{\"a\":1,}", &value));
    EXPECT_FALSE(json::parse("{\"a\":1} trailing", &value));
    EXPECT_FALSE(json::parse("\"unterminated", &value));
    EXPECT_FALSE(json::parse("\"\\ud83c alone\"", &value));
    EXPECT_FALSE(json::parse(std::string(100, '[') + std::string(100, ']'), &value));
}

TEST(Env, ReadsKeysQuotesAndComments)
{
    const Env env =
        parse_env("# a comment\n\nTMDB_API_KEY = abc123  \nexport TMDB_LANGUAGE=\"de-DE\"\n"
                  "DNS_SERVER=192.168.0.1 # the router\nbroken line\r\n");
    EXPECT_EQ(env.get("TMDB_API_KEY"), "abc123");
    EXPECT_EQ(env.get("TMDB_LANGUAGE"), "de-DE");
    EXPECT_EQ(env.get("DNS_SERVER"), "192.168.0.1");
    EXPECT_EQ(env.get("nothing"), "");
}

TEST(Tmdb, RefusesAKeyThatCouldBreakOutOfAUrl)
{
    EXPECT_FALSE(config_from_env(parse_env("")).usable());
    EXPECT_FALSE(config_from_env(parse_env("TMDB_API_KEY=a&b=c")).usable());
    EXPECT_FALSE(config_from_env(parse_env("TMDB_API_KEY=a b")).usable());
    const TmdbConfig config = config_from_env(parse_env("TMDB_API_KEY=0123456789abcdef"));
    EXPECT_TRUE(config.usable());
    EXPECT_EQ(config.language, "en-US");
}

TEST(Tmdb, BuildsRequestsForBothKindsOfKey)
{
    TmdbConfig v3;
    v3.key = "k3";
    const Request a = search_request(v3, "Amélie & Co", 2001);
    EXPECT_NE(a.url.find("query=Am%C3%A9lie%20%26%20Co"), std::string::npos);
    EXPECT_NE(a.url.find("&year=2001"), std::string::npos);
    EXPECT_NE(a.url.find("&api_key=k3"), std::string::npos);
    EXPECT_TRUE(a.headers.empty());

    TmdbConfig v4;
    v4.key = std::string(60, 'x');
    const Request b = search_request(v4, "Dune", 0);
    EXPECT_EQ(b.url.find("api_key"), std::string::npos); // the token goes in a header
    EXPECT_NE(b.headers.find("Authorization: Bearer "), std::string::npos);
    EXPECT_EQ(b.url.find("year="), std::string::npos);
}

TEST(Tmdb, ImagePathsAreChecked)
{
    EXPECT_EQ(image_request("/abc.jpg", 500).url, "https://image.tmdb.org/t/p/w500/abc.jpg");
    EXPECT_TRUE(image_request("abc.jpg", 500).url.empty());
    EXPECT_TRUE(image_request("/../x", 500).url.empty());
    EXPECT_TRUE(image_request("/a b?c", 500).url.empty());
}

TEST(Tmdb, PicksTheFilmOfTheRightYear)
{
    Match match;
    ASSERT_TRUE(pick_match(kAnswer, 2024, &match));
    EXPECT_EQ(match.title, "Dune: Part Two");
    EXPECT_EQ(match.backdrop_path, "/two_bd.jpg");
    ASSERT_TRUE(pick_match(kAnswer, 0, &match));
    EXPECT_EQ(match.poster_path, "/old.jpg"); // no year: the first with a picture
    ASSERT_TRUE(pick_match(kAnswer, 1999, &match));
    EXPECT_EQ(match.poster_path, "/old.jpg");
    EXPECT_FALSE(pick_match(R"({"results":[]})", 0, &match));
    EXPECT_FALSE(pick_match("<html>error</html>", 0, &match));
    EXPECT_FALSE(pick_match(R"({"results":[{"title":"x","poster_path":null}]})", 0, &match));
}

CoverJob dune_job(const std::string &id)
{
    CoverJob job;
    job.id = id;
    job.title = "Dune";
    job.year = 2024;
    return job;
}

TEST(Covers, StoresCoverAndWallpaperAndFillsTheMovie)
{
    FakeWeb web;
    const CoverStore store(temp_directory());
    TmdbConfig config;
    config.key = "k";
    std::string note;
    EXPECT_EQ(fetch_cover(config, web.get(), store, dune_job("aa11"), &note), FetchOutcome::stored);
    EXPECT_TRUE(store.has_poster("aa11"));
    EXPECT_TRUE(store.has_wallpaper("aa11"));

    Movie movie;
    movie.id = "aa11";
    store.fill(&movie);
    EXPECT_EQ(movie.poster, store.poster_path("aa11"));
    EXPECT_EQ(movie.backdrop, store.wallpaper_path("aa11"));

    Movie own;
    own.id = "aa11";
    own.poster = "/data/movies/own.jpg"; // a file beside the film wins
    store.fill(&own);
    EXPECT_EQ(own.poster, "/data/movies/own.jpg");
    EXPECT_EQ(own.backdrop, store.wallpaper_path("aa11"));
}

TEST(Covers, StoresTheDescriptionTidiedAndFillsTheMovie)
{
    FakeWeb web;
    const CoverStore store(temp_directory());
    TmdbConfig config;
    config.key = "k";
    std::string note;
    CoverJob job = dune_job("dd44");
    job.want_poster = false;
    job.want_wallpaper = false; // a film that already has its pictures
    EXPECT_EQ(fetch_cover(config, web.get(), store, job, &note), FetchOutcome::stored);
    EXPECT_EQ(web.images.load(), 0);
    EXPECT_FALSE(store.has_poster("dd44"));

    Movie movie;
    movie.id = "dd44";
    store.fill(&movie);
    EXPECT_EQ(movie.overview, "Paul unites with Chani and the Fremen.");
    // Fetched again, the old text goes with the old pictures.
    store.forget("dd44");
    EXPECT_FALSE(store.has_overview("dd44"));
}

TEST(Covers, AFilmWithoutADescriptionIsNotAskedAgain)
{
    FakeWeb web;
    const CoverStore store(temp_directory());
    TmdbConfig config;
    config.key = "k";
    std::string note;
    CoverJob job = dune_job("ee55");
    job.year = 1984; // the old film: TMDB has no description for it
    EXPECT_EQ(fetch_cover(config, web.get(), store, job, &note), FetchOutcome::stored);
    EXPECT_TRUE(store.has_overview("ee55"));
    Movie movie;
    movie.id = "ee55";
    movie.title = "Dune";
    movie.poster = "/own.jpg";
    movie.backdrop = "/own-fanart.jpg";
    store.fill(&movie);
    EXPECT_TRUE(movie.overview.empty());

    ScanResult scan;
    scan.roots = {"/data/movies"};
    FoundFile file;
    file.path = "/data/movies/Dune (1984).mkv";
    scan.videos = {file};
    Library library = Library::from_scan(scan);
    library.edit_all(
        [&](Movie *m)
        {
            m->poster = "/own.jpg";
            m->backdrop = "/own-fanart.jpg";
        });
    EXPECT_TRUE(missing_covers(library, store).empty()); // it has a cover: not asked at all
    const CoverStore known(temp_directory());
    std::string unused;
    CoverJob again = dune_job(library[0].id);
    again.year = 1984;
    EXPECT_EQ(fetch_cover(config, web.get(), known, again, &unused), FetchOutcome::stored);
    EXPECT_TRUE(missing_covers(library, known).empty());
}

TEST(Covers, ALongOrHostileDescriptionIsCutAndCleaned)
{
    std::string long_text = "Start\\u0007";
    for (int i = 0; i < 700; ++i)
        long_text += " \xC3\xA4rger word"; // multi-byte characters across the cut
    const std::string answer =
        R"({"results":[{"title":"Long","poster_path":"/p.jpg","overview":")" + long_text +
        R"("}]})";
    const HttpGet get = [&](const Request &request, std::size_t, std::string *body, std::string *)
    {
        *body = request.url.find("/search/movie") != std::string::npos
                    ? answer
                    : std::string("\xFF\xD8\xFF\xE0 fake jpeg data");
        return true;
    };
    const CoverStore store(temp_directory());
    TmdbConfig config;
    config.key = "k";
    std::string note;
    CoverJob job;
    job.id = "ff66";
    job.title = "Long";
    EXPECT_EQ(fetch_cover(config, get, store, job, &note), FetchOutcome::stored);
    Movie movie;
    movie.id = "ff66";
    store.fill(&movie);
    EXPECT_LE(movie.overview.size(), 3010u);
    EXPECT_GT(movie.overview.size(), 2000u);
    EXPECT_EQ(movie.overview.substr(movie.overview.size() - 3), "\xE2\x80\xA6");
    EXPECT_EQ(movie.overview.compare(0, 5, "Start"), 0);
    for (const char c : movie.overview)
        EXPECT_GE(static_cast<unsigned char>(c), 0x20) << "a control character survived";
}

TEST(Tmdb, AHitWithOnlyADescriptionIsTheLastResort)
{
    Match match;
    const char *answer =
        R"({"results":[{"title":"Quiet","overview":"Words only."},{"title":"Loud","poster_path":"/l.jpg"}]})";
    ASSERT_TRUE(pick_match(answer, 0, &match));
    EXPECT_EQ(match.title, "Loud"); // a hit with a picture comes first
    ASSERT_TRUE(
        pick_match(R"({"results":[{"title":"Quiet","overview":"Words only."}]})", 0, &match));
    EXPECT_EQ(match.title, "Quiet");
    EXPECT_EQ(match.overview, "Words only.");
    EXPECT_TRUE(match.poster_path.empty());
}

TEST(Covers, ASearchWithoutTheYearIsTriedWhenTheYearFindsNothing)
{
    FakeWeb web;
    const CoverStore store(temp_directory());
    TmdbConfig config;
    config.key = "k";
    std::string note;
    CoverJob job = dune_job("bb22");
    job.title = "Nothing";
    EXPECT_EQ(fetch_cover(config, web.get(), store, job, &note), FetchOutcome::no_match);
    EXPECT_EQ(web.searches.load(), 2); // with the year, then without
    EXPECT_TRUE(store.no_match("bb22"));
    store.forget("bb22");
    EXPECT_FALSE(store.no_match("bb22"));
}

TEST(Covers, OnlyFilmsWithoutACoverAreFetchedAndNotThoseTmdbRejected)
{
    ScanResult scan;
    scan.roots = {"/data/movies"};
    const auto video = [](const char *path, std::uint64_t size)
    {
        FoundFile file;
        file.path = path;
        file.size = size;
        return file;
    };
    scan.videos = {video("/data/movies/A (2001).mkv", 1), video("/data/movies/B (2002).mkv", 2),
                   video("/data/movies/C (2003).mkv", 3)};
    FoundFile poster;
    poster.path = "/data/movies/A (2001)-poster.jpg";
    poster.readable = poster.path;
    scan.images = {poster};
    const Library library = Library::from_scan(scan);
    const CoverStore store(temp_directory());
    store.mark_no_match(library[2].id);

    const std::vector<CoverJob> jobs = missing_covers(library, store);
    // A has a cover of its own and C was rejected before: only B is asked for.
    ASSERT_EQ(jobs.size(), 1u);
    EXPECT_EQ(jobs[0].title, "B");
    EXPECT_TRUE(jobs[0].want_poster);
    EXPECT_TRUE(jobs[0].want_wallpaper);
    EXPECT_TRUE(jobs[0].want_overview);

    // Rejected by TMDB, B is left alone too.
    store.mark_no_match(library[1].id);
    EXPECT_TRUE(missing_covers(library, store).empty());
}

TEST(Covers, ServiceFetchesInTheBackgroundAndReportsWhatArrived)
{
    FakeWeb web;
    TmdbConfig config;
    config.key = "k";
    CoverService service(config, web.get(), temp_directory());
    service.enqueue({dune_job("cc33"), dune_job("dd44")});
    std::vector<CoverService::Done> arrived;
    ASSERT_TRUE(wait_for(
        [&]()
        {
            for (const CoverService::Done &done : service.take_done())
                arrived.push_back(done);
            return arrived.size() == 2;
        }));
    EXPECT_FALSE(arrived[0].replaced);
    EXPECT_TRUE(service.store().has_poster("cc33"));

    service.redo(dune_job("cc33")); // "fetch again": the old pictures go first
    CoverJob again = dune_job("cc33");
    again.again = true;
    service.redo(again);
    ASSERT_TRUE(wait_for(
        [&]()
        {
            for (const CoverService::Done &done : service.take_done())
                if (done.replaced)
                    return true;
            return false;
        }));
    EXPECT_TRUE(service.store().has_poster("cc33"));
}

TEST(Covers, ServiceStopsAfterRepeatedNetworkFailuresAndSaysSo)
{
    FakeWeb web;
    web.offline = true;
    TmdbConfig config;
    config.key = "k";
    CoverService service(config, web.get(), temp_directory());
    std::vector<CoverJob> jobs;
    for (int i = 0; i < 10; ++i)
        jobs.push_back(dune_job("e" + std::to_string(i)));
    service.enqueue(jobs);
    ASSERT_TRUE(wait_for([&]() { return !service.problem().empty(); }));
    EXPECT_NE(service.problem().find("Connection refused"), std::string::npos);
    EXPECT_TRUE(wait_for([&]() { return service.pending() == 0; }));
    EXPECT_EQ(web.searches.load(), 0); // never got through
}

TEST(Covers, ServiceWithoutAKeyAsksForOne)
{
    FakeWeb web;
    TmdbConfig config = config_from_env(parse_env(""));
    CoverService service(config, web.get(), temp_directory());
    service.enqueue({dune_job("ff55")});
    ASSERT_TRUE(wait_for([&]() { return !service.problem().empty(); }));
    EXPECT_NE(service.problem().find("TMDB_API_KEY"), std::string::npos);
    EXPECT_EQ(web.searches.load(), 0);
}

} // namespace
