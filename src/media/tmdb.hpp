// Mediathek - Covers and wallpapers from The Movie Database: requests, answers, local store.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This product uses the TMDB API but is not endorsed or certified by TMDB.

#pragma once

#include "media/library.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hui::media
{

// A ".env" file: KEY=value lines, '#' comments, optional quotes and "export ".
struct Env
{
    std::map<std::string, std::string> values;
    std::string get(const std::string &key) const;
};
Env parse_env(std::string_view text);

struct TmdbConfig
{
    std::string key;                // the v3 API key, or the long v4 read access token
    std::string language = "en-US"; // the language of posters and titles
    std::string dns;                // nameserver for the console; "" = the built-in ones
    std::string problem;            // why the key was refused, when it was
    bool usable() const
    {
        return !key.empty();
    }
};
// TMDB_API_KEY, TMDB_LANGUAGE and DNS_SERVER. A key with characters that do
// not belong in a key is refused: it ends up in a URL or a header.
TmdbConfig config_from_env(const Env &env);

struct Request
{
    std::string url;
    std::string headers; // "Name: value\r\n" lines, or ""
};
Request search_request(const TmdbConfig &config, const std::string &title, int year);
// A picture below TMDB's image server; empty url when `path` is not a plain path.
Request image_request(const std::string &path, int width);

struct Match
{
    std::string title;
    std::string poster_path;    // "/abc.jpg" or ""
    std::string backdrop_path;  // "/abc.jpg" or ""
    std::string overview;       // the description in the configured language, or ""
    double vote_average = 0.0;  // TMDB's user score, 0 to 10
    double vote_count = 0.0;    // how many votes it rests on; 0 means the score means nothing
    std::vector<int> genre_ids; // TMDB's genre numbers, most telling first
};
// The film in a search answer: an exact year first, else the first hit that has a
// picture, else the first that has a description. False when the answer is no
// search answer or has nothing usable.
bool pick_match(std::string_view answer, int year, Match *out);

// Fetches a web page; false (with a sentence in *error) when it cannot.
using HttpGet = std::function<bool(const Request &request, std::size_t max_bytes, std::string *body,
                                   std::string *error)>;

// What was fetched so far: <dir>/<id>.jpg (cover), <id>-wallpaper.jpg,
// <id>.txt (the description; empty when TMDB has none, so it is not asked
// again), <id>.rating (the user score, "0" when TMDB has none), <id>.genres
// (genre numbers separated by commas, "-" when there are none), <id>.title (TMDB's
// title in the configured language, an empty line when it has none) and <id>.none
// for a film TMDB does not know.
class CoverStore
{
  public:
    explicit CoverStore(std::string directory);
    std::string poster_path(const std::string &id) const;
    std::string wallpaper_path(const std::string &id) const;
    bool has_poster(const std::string &id) const;
    bool has_wallpaper(const std::string &id) const;
    std::string overview_path(const std::string &id) const;
    bool has_overview(const std::string &id) const;
    std::string rating_path(const std::string &id) const;
    bool has_rating(const std::string &id) const;
    std::string genres_path(const std::string &id) const;
    bool has_genres(const std::string &id) const;
    std::string title_path(const std::string &id) const;
    bool has_title(const std::string &id) const;
    bool no_match(const std::string &id) const;
    void mark_no_match(const std::string &id) const;
    // Forgets the film: the next fetch starts from nothing.
    void forget(const std::string &id) const;
    // Gives a movie the pictures held here where it has none of its own, and
    // the description and title held here; with no title held, the file's.
    void fill(Movie *movie) const;
    const std::string &directory() const
    {
        return directory_;
    }

  private:
    std::string directory_;
};

struct CoverJob
{
    std::string id;
    std::string title; // what TMDB is searched for: the title from the file name
    int year = 0;
    bool want_poster = true;
    bool want_wallpaper = true;
    bool want_overview = true;
    bool want_facts = true; // the user score, the genres and the title
    bool again = false;     // fetched on request: pictures shown before must be reloaded
};

// What a library still lacks: films with no cover that TMDB has not already
// turned down. Each is asked for its poster, and for the wallpaper, description
// and score, genres and title it has none of. A film that has a cover is asked for
// nothing, except once for its score, genres and title if it was fetched before
// they were kept.
std::vector<CoverJob> missing_covers(const Library &library, const CoverStore &store);

enum class FetchOutcome : unsigned char
{
    stored,   // at least the wanted pictures or description that exist were saved
    no_match, // TMDB does not know the film (remembered)
    network,  // could not reach the service, or it answered badly
    no_key,   // there is no usable key
};
FetchOutcome fetch_cover(const TmdbConfig &config, const HttpGet &get, const CoverStore &store,
                         const CoverJob &job, std::string *note);

} // namespace hui::media
