// Mediathek - Covers and wallpapers from The Movie Database: requests, answers, local store.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/tmdb.hpp"

#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/save_file.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace hui::media
{

namespace
{

constexpr const char *kApi = "https://api.themoviedb.org/3";
constexpr const char *kImages = "https://image.tmdb.org/t/p";
constexpr std::size_t kMaxAnswer = 256u << 10;
// A season lists every episode with its cast and crew: a long one is far bigger than a search.
constexpr std::size_t kMaxSeasonAnswer = 2u << 20;
constexpr std::size_t kMaxPicture = 4u << 20;
constexpr std::size_t kMaxOverview = 3000; // a description is a paragraph, not an essay
constexpr std::size_t kMaxTitle = 200;     // the longest film titles have about 180 characters
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

// The score file: "7.8", or "0" when TMDB has no votes. A damaged file means no score.
float parse_rating(const std::string &text)
{
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !(value > 0.0))
        return 0.0f;
    return static_cast<float>(std::min(value, 10.0));
}

// The genres file: numbers separated by commas, "-" for none.
std::vector<int> parse_genres(const std::string &text)
{
    std::vector<int> ids;
    const char *at = text.c_str();
    while (*at != '\0' && ids.size() < 8)
    {
        char *end = nullptr;
        const long value = std::strtol(at, &end, 10);
        if (end == at)
        {
            ++at; // a comma, a "-" or a stray character
            continue;
        }
        if (value > 0 && value < 1000000)
            ids.push_back(static_cast<int>(value));
        at = end;
    }
    return ids;
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

// A title as the screen gets it: one line, no controls. One longer than any film's is no
// title (an answer that is not what it claims), and the file's is shown instead.
std::string clean_title(std::string_view raw)
{
    std::string text = clean_overview(raw, false);
    if (text.size() > kMaxTitle)
        text.clear();
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

namespace
{

void authorize(const TmdbConfig &config, Request *request)
{
    if (is_long_token(config.key))
        request->headers = "Authorization: Bearer " + config.key + "\r\n";
    else
        request->url += "&api_key=" + config.key;
}

} // namespace

Request search_request(const TmdbConfig &config, const std::string &title, int year, bool show)
{
    Request request;
    request.url = std::string(kApi) + (show ? "/search/tv" : "/search/movie") +
                  "?query=" + percent_encode(title) + "&language=" + config.language +
                  "&include_adult=false";
    if (year > 0)
        request.url += (show ? "&first_air_date_year=" : "&year=") + std::to_string(year);
    authorize(config, &request);
    return request;
}

Request season_request(const TmdbConfig &config, int tmdb_id, int season)
{
    Request request;
    request.url = std::string(kApi) + "/tv/" + std::to_string(tmdb_id) + "/season/" +
                  std::to_string(season) + "?language=" + config.language;
    authorize(config, &request);
    return request;
}

std::vector<int> tv_genres_as_films(const std::vector<int> &genres)
{
    std::vector<int> out;
    const auto add = [&out](int genre)
    {
        if (std::find(out.begin(), out.end(), genre) == out.end())
            out.push_back(genre);
    };
    for (const int genre : genres)
    {
        switch (genre)
        {
        case 10759: // Action & Adventure
            add(28);
            add(12);
            break;
        case 10765: // Sci-Fi & Fantasy
            add(878);
            add(14);
            break;
        case 10768: // War & Politics
            add(10752);
            break;
        case 10762: // Kids
            add(10751);
            break;
        default:
            add(genre);
            break;
        }
    }
    return out;
}

bool parse_season(std::string_view answer, SeasonAnswer *out)
{
    json::Value root;
    if (!json::parse(answer, &root) || root.kind != json::Value::Kind::object)
        return false;
    const json::Value *episodes = root.get("episodes");
    if (episodes == nullptr || episodes->kind != json::Value::Kind::array)
        return false;
    *out = SeasonAnswer();
    out->overview = root.text("overview");
    for (const json::Value &episode : episodes->items)
    {
        const json::Value *number = episode.get("episode_number");
        if (episode.kind != json::Value::Kind::object || number == nullptr ||
            number->kind != json::Value::Kind::number || number->number < 0.0 ||
            number->number > 9999.0)
            continue;
        SeasonAnswer::Item item;
        item.number = static_cast<int>(number->number);
        item.name = episode.text("name");
        item.overview = episode.text("overview");
        out->episodes.push_back(std::move(item));
    }
    return true;
}

Request image_request(const std::string &path, int width)
{
    Request request;
    if (plain_path(path))
        request.url = std::string(kImages) + "/w" + std::to_string(width) + path;
    return request;
}

bool pick_match(std::string_view answer, int year, Match *out, bool show)
{
    const char *title_key = show ? "name" : "title";
    const char *date_key = show ? "first_air_date" : "release_date";
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
        if (year > 0 && hit.text(date_key).compare(0, 4, std::to_string(year)) == 0)
        {
            best = &hit;
            break;
        }
    }
    if (best == nullptr)
        best = text_only;
    if (best == nullptr)
        return false;
    *out = Match(); // nothing of an earlier answer stays
    if (const json::Value *id = best->get("id"); id != nullptr &&
                                                 id->kind == json::Value::Kind::number &&
                                                 id->number > 0.0 && id->number < 1e9)
        out->id = static_cast<int>(id->number);
    out->title = best->text(title_key);
    out->poster_path = best->text("poster_path");
    out->backdrop_path = best->text("backdrop_path");
    out->overview = best->text("overview");
    if (const json::Value *average = best->get("vote_average");
        average != nullptr && average->kind == json::Value::Kind::number)
        out->vote_average = average->number;
    if (const json::Value *ids = best->get("genre_ids");
        ids != nullptr && ids->kind == json::Value::Kind::array)
        for (const json::Value &id : ids->items)
            if (id.kind == json::Value::Kind::number && id.number > 0.0 && id.number < 1e6)
                out->genre_ids.push_back(static_cast<int>(id.number));
    if (const json::Value *votes = best->get("vote_count");
        votes != nullptr && votes->kind == json::Value::Kind::number)
        out->vote_count = votes->number;
    if (show)
        out->genre_ids = tv_genres_as_films(out->genre_ids);
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
std::string CoverStore::rating_path(const std::string &id) const
{
    return directory_ + "/" + id + ".rating";
}
bool CoverStore::has_rating(const std::string &id) const
{
    return exists(rating_path(id));
}
std::string CoverStore::genres_path(const std::string &id) const
{
    return directory_ + "/" + id + ".genres";
}
bool CoverStore::has_genres(const std::string &id) const
{
    return exists(genres_path(id));
}
std::string CoverStore::title_path(const std::string &id) const
{
    return directory_ + "/" + id + ".title";
}
bool CoverStore::has_title(const std::string &id) const
{
    return exists(title_path(id));
}
std::string CoverStore::season_path(const std::string &id, int season) const
{
    return directory_ + "/" + id + "-s" + std::to_string(season) + ".txt";
}
bool CoverStore::has_season(const std::string &id, int season) const
{
    return exists(season_path(id, season));
}
std::string CoverStore::tmdb_id_path(const std::string &id) const
{
    return directory_ + "/" + id + ".tmdb";
}
int CoverStore::tmdb_id(const std::string &id) const
{
    std::string text;
    if (!save::read_file(tmdb_id_path(id), &text, 32))
        return 0;
    const long value = std::strtol(text.c_str(), nullptr, 10);
    return value > 0 && value < 1000000000 ? static_cast<int>(value) : 0;
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
    unlink(rating_path(id).c_str());
    unlink(genres_path(id).c_str());
    unlink(title_path(id).c_str());
    unlink(tmdb_id_path(id).c_str());
    unlink((directory_ + "/" + id + ".none").c_str());
}
void CoverStore::forget_season(const std::string &id, int season,
                               const std::vector<std::string> &episodes) const
{
    unlink(season_path(id, season).c_str());
    for (const std::string &episode : episodes)
    {
        unlink(overview_path(episode).c_str());
        unlink(title_path(episode).c_str());
    }
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
    if (has_rating(movie->id) && save::read_file(rating_path(movie->id), &text, 16))
        movie->rating = parse_rating(text);
    if (has_genres(movie->id) && save::read_file(genres_path(movie->id), &text, 128))
        movie->genres = parse_genres(text);
    // TMDB's title keeps the colons, commas and dashes a file name loses. Without one (not
    // fetched yet, TMDB has none, or it was forgotten for a new fetch) the file's stands.
    std::string title;
    if (has_title(movie->id) && save::read_file(title_path(movie->id), &text, kMaxTitle + 16))
        title = clean_title(text);
    if (!title.empty())
        movie->title = std::move(title);
    else if (!movie->file_title.empty())
        movie->title = movie->file_title;
    for (Season &season : movie->seasons)
    {
        if (has_season(movie->id, season.number) &&
            save::read_file(season_path(movie->id, season.number), &text, kMaxOverview + 16))
            season.overview = clean_overview(text, false);
        for (Episode &episode : season.episodes)
        {
            if (has_overview(episode.id) &&
                save::read_file(overview_path(episode.id), &text, kMaxOverview + 16))
                episode.overview = clean_overview(text, false);
            std::string name;
            if (has_title(episode.id) &&
                save::read_file(title_path(episode.id), &text, kMaxTitle + 16))
                name = clean_title(text);
            episode.title = name.empty() ? episode.file_title : std::move(name);
        }
    }
}

std::vector<SeasonJob> season_jobs(const Movie &show)
{
    std::vector<SeasonJob> jobs;
    for (const Season &season : show.seasons)
    {
        SeasonJob job;
        job.number = season.number;
        for (const Episode &episode : season.episodes)
            job.episodes.emplace_back(episode.number, episode.id);
        jobs.push_back(std::move(job));
    }
    return jobs;
}

std::vector<CoverJob> missing_covers(const Library &library, const CoverStore &store)
{
    std::vector<CoverJob> jobs;
    for (const Movie &movie : library.movies())
    {
        if (store.no_match(movie.id))
            continue;
        CoverJob job;
        job.id = movie.id;
        job.title = movie.file_title.empty() ? movie.title : movie.file_title;
        job.year = movie.year;
        job.show = movie.show;
        job.want_facts = !store.has_rating(movie.id) || !store.has_genres(movie.id) ||
                         !store.has_title(movie.id);
        // A season is asked for when it, or one of its episodes, has no description yet:
        // a season or an episode that came after the last fetch.
        for (SeasonJob &season : season_jobs(movie))
        {
            bool wanted = !store.has_season(movie.id, season.number);
            for (const auto &episode : season.episodes)
                wanted = wanted || !store.has_overview(episode.second);
            if (wanted)
                job.seasons.push_back(std::move(season));
        }
        if (!movie.poster.empty())
        {
            // A film that has a cover (its own, or one fetched earlier and filled in by
            // CoverStore::fill) is done: asking TMDB again on every scan and start would
            // cost a request per film that lacks a wallpaper or description TMDB never
            // had. Only the score, kept since 0.3.0, and the title, kept after 0.8.0, are
            // asked for once, whether TMDB has them or not (the files say so then).
            if (!job.want_facts && job.seasons.empty())
                continue;
            job.want_poster = false;
            job.want_wallpaper = false;
            job.want_overview = false;
        }
        else
        {
            job.want_wallpaper = movie.backdrop.empty();
            job.want_overview = !store.has_overview(movie.id);
        }
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

// The descriptions of one season and of its episodes, and the episodes' names. What TMDB
// does not have (a season it does not list, an episode it does not know) is stored as an
// empty line, so it is not asked for again.
FetchOutcome fetch_season(const TmdbConfig &config, const HttpGet &get, const CoverStore &store,
                          const std::string &id, int tmdb_id, const SeasonJob &season,
                          std::string *note)
{
    std::string answer;
    std::string error;
    SeasonAnswer found;
    if (get(season_request(config, tmdb_id, season.number), kMaxSeasonAnswer, &answer, &error))
    {
        if (!parse_season(answer, &found))
        {
            *note = i18n::tr(i18n::Text::tmdb_bad_answer);
            return FetchOutcome::network;
        }
    }
    else if (error.find("404") == std::string::npos)
    {
        *note = error;
        return FetchOutcome::network;
    }
    const auto save_text = [&](const std::string &path, const std::string &text) -> bool
    {
        const std::string failure = save::write_atomic(path, text.empty() ? "\n" : text);
        if (!failure.empty())
            *note = i18n::format(i18n::Text::tmdb_cannot_save_description, failure);
        return failure.empty();
    };
    if (!save_text(store.season_path(id, season.number), clean_overview(found.overview)))
        return FetchOutcome::network;
    for (const auto &episode : season.episodes)
    {
        const SeasonAnswer::Item *item = nullptr;
        for (const SeasonAnswer::Item &candidate : found.episodes)
            if (candidate.number == episode.first)
                item = &candidate;
        if (!save_text(store.title_path(episode.second),
                       item != nullptr ? clean_title(item->name) : std::string()) ||
            !save_text(store.overview_path(episode.second),
                       item != nullptr ? clean_overview(item->overview) : std::string()))
            return FetchOutcome::network;
    }
    return FetchOutcome::stored;
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
    // A series whose seasons alone are wanted is not searched again: TMDB's number for it
    // was kept the first time.
    const bool search = !job.show || job.want_poster || job.want_wallpaper || job.want_overview ||
                        job.want_facts || store.tmdb_id(job.id) == 0;
    if (search)
    {
        bool found = false;
        // A wrong year in a file name must not hide the film: ask again without it.
        const int years[2] = {job.year, 0};
        for (int attempt = 0; attempt < (job.year > 0 ? 2 : 1) && !found; ++attempt)
        {
            std::string answer;
            std::string error;
            if (!get(search_request(config, job.title, years[attempt], job.show), kMaxAnswer,
                     &answer, &error))
            {
                *note = error;
                return FetchOutcome::network;
            }
            found = pick_match(answer, years[attempt], &match, job.show);
        }
        if (!found)
        {
            store.mark_no_match(job.id);
            return FetchOutcome::no_match;
        }
        if (job.show && match.id > 0)
            save::write_atomic(store.tmdb_id_path(job.id), std::to_string(match.id));
    }
    else
    {
        match.id = store.tmdb_id(job.id);
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
    if (job.want_facts)
    {
        // "0" and "-" record that TMDB has no usable score or genres, so the film is
        // not asked again.
        char text[16] = "0";
        if (match.vote_count > 0.0 && match.vote_average > 0.0)
            std::snprintf(text, sizeof(text), "%.1f", std::min(match.vote_average, 10.0));
        std::string genres;
        for (const int genre : match.genre_ids)
            genres += (genres.empty() ? "" : ",") + std::to_string(genre);
        // An empty line records that TMDB has no title the app can show: the file's stays.
        const std::string title = clean_title(match.title);
        std::string failure = save::write_atomic(store.rating_path(job.id), text);
        if (failure.empty())
            failure = save::write_atomic(store.genres_path(job.id), genres.empty() ? "-" : genres);
        if (failure.empty())
            failure = save::write_atomic(store.title_path(job.id), title.empty() ? "\n" : title);
        if (!failure.empty())
        {
            *note = i18n::format(i18n::Text::tmdb_cannot_save_description, failure);
            return stored ? FetchOutcome::stored : FetchOutcome::network;
        }
        stored = true;
    }
    if (job.show && match.id > 0)
    {
        for (const SeasonJob &season : job.seasons)
        {
            const FetchOutcome outcome =
                fetch_season(config, get, store, job.id, match.id, season, note);
            if (outcome != FetchOutcome::stored)
                return stored ? FetchOutcome::stored : outcome;
            stored = true;
        }
    }
    if (!stored)
    {
        store.mark_no_match(job.id); // known, but without what was asked for
        return FetchOutcome::no_match;
    }
    return FetchOutcome::stored;
}

} // namespace hui::media
