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
    std::string poster_path;   // "/abc.jpg" or ""
    std::string backdrop_path; // "/abc.jpg" or ""
    std::string overview;      // the description in the configured language, or ""
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
// again) and <id>.none for a film TMDB does not know.
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
    bool no_match(const std::string &id) const;
    void mark_no_match(const std::string &id) const;
    // Forgets the film: the next fetch starts from nothing.
    void forget(const std::string &id) const;
    // Gives a movie the pictures held here where it has none of its own, and
    // the description held here.
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
    std::string title;
    int year = 0;
    bool want_poster = true;
    bool want_wallpaper = true;
    bool want_overview = true;
    bool again = false; // fetched on request: pictures shown before must be reloaded
};

// What a library still lacks: films with no cover that TMDB has not already
// turned down. Each is asked for its poster, and for the wallpaper and
// description it has none of; a film that has a cover is never asked again.
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
