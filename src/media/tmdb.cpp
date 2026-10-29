// Mediathek - Covers and wallpapers from The Movie Database: requests, answers, local store.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/tmdb.hpp"

#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/save_file.hpp"

#include <cctype>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace hui::media
{

namespace
{

constexpr const char *kApi = "https://api.themoviedb.org/3";
constexpr const char *kImages = "https://image.tmdb.org/t/p";
constexpr std::size_t kMaxAnswer = 256u << 10;
constexpr std::size_t kMaxPicture = 4u << 20;
constexpr std::size_t kMaxOverview = 3000; // a description is a paragraph, not an essay
constexpr int kPosterWidth = 500;
constexpr int kWallpaperWidth = 1280;

std::string trim(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return std::string(text.substr(begin, end - begin));
}

std::string percent_encode(const std::string &text)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const char c : text)
    {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out.push_back(c);
        }
        else
        {
            out.push_back('%');
            out.push_back(hex[u >> 4]);
            out.push_back(hex[u & 15]);
        }
    }
    return out;
}

bool plain_token(const std::string &text)
{
    if (text.empty())
        return false;
    for (const char c : text)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.')
            return false;
    return true;
}

bool plain_path(const std::string &path)
{
    if (path.size() < 2 || path[0] != '/' || path.find("..") != std::string::npos)
        return false;
    for (const char c : path)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.' &&
            c != '/')
            return false;
    return true;
}

bool looks_like_picture(const std::string &data)
{
    return data.size() > 16 &&
           ((data[0] == '\xFF' && data[1] == '\xD8') || data.compare(0, 4, "\x89PNG") == 0);
}

bool exists(const std::string &path)
{
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

// One line of text: line breaks and runs of blanks become one space, controls
// go, and a long text ends at a word with an ellipsis (never inside a UTF-8
// character). The answer comes from the network and ends up on screen.
std::string clean_overview(std::string_view raw, bool shorten = true)
{
    std::string text;
    bool space = true; // swallows leading blanks
    for (const char c : raw)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n')
        {
            if (!space)
                text.push_back(' ');
            space = true;
        }
        else if (byte >= 0x20 && byte != 0x7F)
        {
            text.push_back(c);
            space = false;
        }
    }
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    if (shorten && text.size() > kMaxOverview)
    {
        std::size_t cut = kMaxOverview;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut; // not in the middle of a character
        const std::size_t word = text.rfind(' ', cut);
        text.resize(word != std::string::npos && word > cut / 2 ? word : cut);
        text += "\xE2\x80\xA6";
    }
    return text;
}

bool is_long_token(const std::string &key)
{
    return key.size() > 40; // the v4 "read access token" is a JWT; v3 keys have 32 characters
}

} // namespace

std::string Env::get(const std::string &key) const
{
    const auto it = values.find(key);
    return it == values.end() ? std::string() : it->second;
}

Env parse_env(std::string_view text)
{
    Env env;
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos)
            end = text.size();
        std::string line = trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == '#')
            continue;
        if (line.compare(0, 7, "export ") == 0)
            line = trim(line.substr(7));
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        std::string name = trim(line.substr(0, equals));
        std::string value = trim(line.substr(equals + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        else if (const std::size_t hash = value.find(" #"); hash != std::string::npos)
            value = trim(value.substr(0, hash)); // a trailing comment
        if (!name.empty())
            env.values[name] = value;
    }
    return env;
}

TmdbConfig config_from_env(const Env &env)
{
    TmdbConfig config;
    // Descriptions come in the language of the app unless the owner chose one.
    config.language = i18n::tmdb_language(i18n::language());
    const std::string key = env.get("TMDB_API_KEY");
    if (key.empty())
        config.problem = i18n::tr(i18n::Text::tmdb_no_key);
    else if (!plain_token(key))
        config.problem = i18n::tr(i18n::Text::tmdb_bad_key);
    else
        config.key = key;
    if (const std::string language = env.get("TMDB_LANGUAGE"); plain_token(language))
        config.language = language;
    config.dns = env.get("DNS_SERVER");
    return config;
}

Request search_request(const TmdbConfig &config, const std::string &title, int year)
{
    Request request;
    request.url = std::string(kApi) + "/search/movie?query=" + percent_encode(title) +
                  "&language=" + config.language + "&include_adult=false";
    if (year > 0)
        request.url += "&year=" + std::to_string(year);
    if (is_long_token(config.key))
        request.headers = "Authorization: Bearer " + config.key + "\r\n";
    else
        request.url += "&api_key=" + config.key;
    return request;
}

Request image_request(const std::string &path, int width)
{
    Request request;
    if (plain_path(path))
        request.url = std::string(kImages) + "/w" + std::to_string(width) + path;
    return request;
}

bool pick_match(std::string_view answer, int year, Match *out)
{
    json::Value root;
    if (!json::parse(answer, &root))
        return false;
    const json::Value *results = root.get("results");
    if (results == nullptr || results->kind != json::Value::Kind::array)
        return false;
    const json::Value *best = nullptr;
    const json::Value *text_only = nullptr; // no picture, but a description
    for (const json::Value &hit : results->items)
    {
        if (hit.kind != json::Value::Kind::object)
            continue;
        if (hit.text("poster_path").empty() && hit.text("backdrop_path").empty())
        {
            if (text_only == nullptr && !hit.text("overview").empty())
                text_only = &hit;
            continue;
        }
        if (best == nullptr)
            best = &hit;
        if (year > 0 && hit.text("release_date").compare(0, 4, std::to_string(year)) == 0)
        {
            best = &hit;
            break;
        }
    }
    if (best == nullptr)
        best = text_only;
    if (best == nullptr)
        return false;
    out->title = best->text("title");
    out->poster_path = best->text("poster_path");
    out->backdrop_path = best->text("backdrop_path");
    out->overview = best->text("overview");
    return true;
}

CoverStore::CoverStore(std::string directory) : directory_(std::move(directory))
{
}

std::string CoverStore::poster_path(const std::string &id) const
{
    return directory_ + "/" + id + ".jpg";
}
std::string CoverStore::wallpaper_path(const std::string &id) const
{
    return directory_ + "/" + id + "-wallpaper.jpg";
}
bool CoverStore::has_poster(const std::string &id) const
{
    return exists(poster_path(id));
}
bool CoverStore::has_wallpaper(const std::string &id) const
{
    return exists(wallpaper_path(id));
}
std::string CoverStore::overview_path(const std::string &id) const
{
    return directory_ + "/" + id + ".txt";
}
bool CoverStore::has_overview(const std::string &id) const
{
    return exists(overview_path(id));
}
bool CoverStore::no_match(const std::string &id) const
{
    struct stat info{};
    return stat((directory_ + "/" + id + ".none").c_str(), &info) == 0;
}
void CoverStore::mark_no_match(const std::string &id) const
{
    save::write_atomic(directory_ + "/" + id + ".none", "-");
}
void CoverStore::forget(const std::string &id) const
{
    unlink(poster_path(id).c_str());
    unlink(wallpaper_path(id).c_str());
    unlink(overview_path(id).c_str());
    unlink((directory_ + "/" + id + ".none").c_str());
}
void CoverStore::fill(Movie *movie) const
{
    if (movie->poster.empty() && has_poster(movie->id))
        movie->poster = poster_path(movie->id);
    if (movie->backdrop.empty() && has_wallpaper(movie->id))
        movie->backdrop = wallpaper_path(movie->id);
    std::string text;
    if (has_overview(movie->id) &&
        save::read_file(overview_path(movie->id), &text, kMaxOverview + 16))
        movie->overview = clean_overview(text, false);
}

std::vector<CoverJob> missing_covers(const Library &library, const CoverStore &store)
{
    std::vector<CoverJob> jobs;
    for (const Movie &movie : library.movies())
    {
        // A film that has a cover (its own, or one fetched earlier and filled in by
        // CoverStore::fill) is done: asking TMDB again on every scan and start would
        // cost a request per film that lacks a wallpaper or description TMDB never had.
        if (!movie.poster.empty() || store.no_match(movie.id))
            continue;
        CoverJob job;
        job.id = movie.id;
        job.title = movie.title;
        job.year = movie.year;
        job.want_wallpaper = movie.backdrop.empty();
        job.want_overview = !store.has_overview(movie.id);
        jobs.push_back(std::move(job));
    }
    return jobs;
}

namespace
{

// Downloads one picture into `path`. False (with *note) when it cannot.
bool download_picture(const HttpGet &get, const std::string &tmdb_path, int width,
                      const std::string &path, std::string *note)
{
    const Request request = image_request(tmdb_path, width);
    if (request.url.empty())
        return false;
    std::string body;
    std::string error;
    if (!get(request, kMaxPicture, &body, &error))
    {
        *note = error;
        return false;
    }
    if (!looks_like_picture(body))
    {
        *note = i18n::tr(i18n::Text::tmdb_not_a_picture);
        return false;
    }
    const std::string failure = save::write_atomic(path, body);
    if (!failure.empty())
    {
        *note = i18n::format(i18n::Text::tmdb_cannot_save_cover, failure);
        return false;
    }
    return true;
}

} // namespace

FetchOutcome fetch_cover(const TmdbConfig &config, const HttpGet &get, const CoverStore &store,
                         const CoverJob &job, std::string *note)
{
    if (!config.usable())
    {
        *note = config.problem.empty() ? i18n::tr(i18n::Text::tmdb_no_key) : config.problem;
        return FetchOutcome::no_key;
    }
    Match match;
    bool found = false;
    // A wrong year in a file name must not hide the film: ask again without it.
    const int years[2] = {job.year, 0};
    for (int attempt = 0; attempt < (job.year > 0 ? 2 : 1) && !found; ++attempt)
    {
        std::string answer;
        std::string error;
        if (!get(search_request(config, job.title, years[attempt]), kMaxAnswer, &answer, &error))
        {
            *note = error;
            return FetchOutcome::network;
        }
        found = pick_match(answer, years[attempt], &match);
    }
    if (!found)
    {
        store.mark_no_match(job.id);
        return FetchOutcome::no_match;
    }
    bool stored = false;
    if (job.want_poster && !match.poster_path.empty())
    {
        if (!download_picture(get, match.poster_path, kPosterWidth, store.poster_path(job.id),
                              note))
            return FetchOutcome::network;
        stored = true;
    }
    if (job.want_wallpaper && !match.backdrop_path.empty())
    {
        if (!download_picture(get, match.backdrop_path, kWallpaperWidth,
                              store.wallpaper_path(job.id), note))
            return stored ? FetchOutcome::stored : FetchOutcome::network;
        stored = true;
    }
    if (job.want_overview)
    {
        // An empty file records that TMDB has no description in this language.
        const std::string text = clean_overview(match.overview);
        const std::string failure =
            save::write_atomic(store.overview_path(job.id), text.empty() ? "\n" : text);
        if (!failure.empty())
        {
            *note = i18n::format(i18n::Text::tmdb_cannot_save_description, failure);
            return stored ? FetchOutcome::stored : FetchOutcome::network;
        }
        stored = true;
    }
    if (!stored)
    {
        store.mark_no_match(job.id); // known, but without what was asked for
        return FetchOutcome::no_match;
    }
    return FetchOutcome::stored;
}

} // namespace hui::media
