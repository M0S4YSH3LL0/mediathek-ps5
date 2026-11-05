// Mediathek - The movie library: files found on the console's drives and their artwork.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/library.hpp"

#include "core/i18n.hpp"
#include "core/save_file.hpp"
#include "media/mediainfo.hpp"
#include "media/title.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <set>
#include <sys/stat.h>
#include <utility>

#include <dirent.h>

namespace hui::media
{

namespace
{

constexpr int kMaxDepth = 5;
constexpr std::size_t kMaxSubtitles = 8; // a film with more is a pack; the pad cycles through them

std::vector<std::string_view> split(std::string_view line, char separator)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t at = line.find(separator, start);
        if (at == std::string_view::npos)
        {
            parts.push_back(line.substr(start));
            return parts;
        }
        parts.push_back(line.substr(start, at - start));
        start = at + 1;
    }
}

std::uint64_t to_u64(std::string_view text)
{
    return std::strtoull(std::string(text).c_str(), nullptr, 10);
}

std::string make_id(const std::string &file_name, std::uint64_t size)
{
    char id[24];
    std::snprintf(id, sizeof(id), "%08x",
                  save::crc32(lower(file_name) + ":" + std::to_string(size)));
    return id;
}

// Which of two qualities is the better: a series shows its best.
int quality_rank(const std::string &quality)
{
    if (quality == "4K")
        return 4;
    if (quality == "1080p")
        return 3;
    if (quality == "720p")
        return 2;
    return quality == "SD" ? 1 : 0;
}

bool is_generic_name(const std::string &base)
{
    const std::string key = lower(base);
    return key == "movie" || key == "video" || key == "main" || key == "film" || key == "title" ||
           key.size() < 3;
}

// Pairs sidecar files with one video. `alone` says the folder holds just this
// video, which is what lets a bare "poster.jpg" belong to it.
const FoundFile *pick(const std::vector<const FoundFile *> &candidates, const std::string &base,
                      std::initializer_list<const char *> suffixes,
                      std::initializer_list<const char *> bare, bool alone)
{
    for (const FoundFile *file : candidates)
    {
        const std::string name = lower(media::base_name(file->path));
        for (const char *suffix : suffixes)
            if (name == lower(base) + suffix)
                return file;
    }
    if (alone)
        for (const FoundFile *file : candidates)
        {
            const std::string name = lower(media::base_name(file->path));
            for (const char *word : bare)
                if (name == word)
                    return file;
        }
    return nullptr;
}

void walk(const std::string &directory, int depth, ScanResult *out)
{
    DIR *dir = opendir(directory.c_str());
    if (dir == nullptr)
        return;
    std::vector<std::string> names;
    while (const dirent *entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || name.empty() || name[0] == '.')
            continue;
        names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (const std::string &name : names)
    {
        const std::string path = directory + "/" + name;
        struct stat info{};
        if (stat(path.c_str(), &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode))
        {
            if (depth < kMaxDepth)
                walk(path, depth + 1, out);
            continue;
        }
        FoundFile file;
        file.path = path;
        file.readable = path;
        file.size = static_cast<std::uint64_t>(info.st_size);
        file.mtime = static_cast<std::int64_t>(info.st_mtime);
        if (is_video_file(name))
            out->videos.push_back(file);
        else if (is_image_file(name))
            out->images.push_back(file);
        else if (is_subtitle_file(name))
            out->subtitles.push_back(file);
    }
}

} // namespace

bool parse_index(std::string_view text, ScanResult *out)
{
    *out = ScanResult{};
    bool header = false;
    bool complete = false;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty())
            continue;
        if (!header)
        {
            if (line != "mediathek-index 1")
                return false;
            header = true;
            continue;
        }
        if (line == "end")
        {
            complete = true;
            break;
        }
        const std::vector<std::string_view> parts = split(line, '\t');
        if (parts.size() < 2 || parts[0].size() != 1)
            continue;
        FoundFile file;
        file.path = std::string(parts[1]);
        switch (parts[0][0])
        {
        case 'R':
            out->roots.push_back(file.path);
            break;
        case 'V':
            if (parts.size() < 4)
                break;
            file.readable = file.path;
            file.size = to_u64(parts[2]);
            file.mtime = static_cast<std::int64_t>(to_u64(parts[3]));
            out->videos.push_back(std::move(file));
            break;
        case 'I':
        case 'S':
            file.readable =
                parts.size() > 2 && !parts[2].empty() ? std::string(parts[2]) : file.path;
            (parts[0][0] == 'I' ? out->images : out->subtitles).push_back(std::move(file));
            break;
        default:
            break;
        }
    }
    return header && complete;
}

std::vector<std::string> scan_drives()
{
    // The same drives, in the same order, as main() in helper/mt-scan.c.
    std::vector<std::string> drives = {"/data", ""};
    for (int i = 0; i < 8; ++i)
        drives.push_back("/mnt/usb" + std::to_string(i));
    for (int i = 0; i < 2; ++i)
        drives.push_back("/mnt/ext" + std::to_string(i));
    return drives;
}

std::vector<std::string> default_roots()
{
    // The same folders, in the same order, as main() in helper/mt-scan.c.
    std::vector<std::string> roots;
    for (const std::string &drive : scan_drives())
        for (const char *folder :
             {"movies", "Movies", "mediathek/movies", "mediathek/Movies", "tvshows", "TVShows",
              "TV Shows", "series", "Series", "mediathek/tvshows", "mediathek/series"})
            roots.push_back(drive + "/" + folder);
    return roots;
}

ScanResult walk_folders(const std::vector<std::string> &roots)
{
    ScanResult result;
    // A USB or extended drive ignores letter case, so "movies" and "Movies"
    // can be one folder; scanning it twice would list every film twice.
    std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
    for (const std::string &root : roots)
    {
        struct stat info{};
        if (stat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
            continue;
        if (!seen.emplace(info.st_dev, info.st_ino).second)
            continue;
        result.roots.push_back(root);
        walk(root, 0, &result);
    }
    return result;
}

std::string drive_label(std::string_view path)
{
    const auto number_after = [&](std::string_view prefix) -> int
    {
        if (path.substr(0, prefix.size()) != prefix)
            return -1;
        const std::string_view rest = path.substr(prefix.size());
        if (rest.empty() || rest[0] < '0' || rest[0] > '9')
            return -1;
        return rest[0] - '0';
    };
    if (const int usb = number_after("/mnt/usb"); usb >= 0)
        return i18n::format(i18n::Text::drive_usb, std::to_string(usb + 1));
    if (const int ext = number_after("/mnt/ext"); ext >= 0)
        return i18n::format(i18n::Text::drive_extended, std::to_string(ext + 1));
    if (path.substr(0, 6) == "/data/")
        return i18n::tr(i18n::Text::drive_internal);
    return i18n::tr(i18n::Text::drive_library);
}

std::vector<int> subtitle_order(const std::vector<SubtitleChoice> &tracks,
                                std::string_view preferred)
{
    const auto rank = [&](int index)
    {
        const SubtitleChoice &track = tracks[static_cast<std::size_t>(index)];
        const bool wanted = !preferred.empty() && track.language == preferred;
        return (wanted ? 0 : 2) + (track.embedded ? 0 : 1);
    };
    std::vector<int> order(tracks.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return rank(a) < rank(b); });
    return order;
}

int preferred_audio_track(const std::vector<AudioChoice> &tracks, std::string_view preferred)
{
    // A wanted language falls back to English because it is the language a film most often
    // has besides its own. Automatic, and a film with neither, leave it to the file: FFmpeg's
    // pick weighs the default flag and leaves out tracks for the impaired, and equal ranks
    // keep the first track.
    int best = -1;
    int best_rank = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i)
    {
        const AudioChoice &track = tracks[i];
        const int tier = preferred.empty()             ? 2
                         : track.language == preferred ? 0
                         : track.language == "eng"     ? 1
                                                       : 2;
        const int rank = tier < 2 ? tier * 4 + (track.extra ? 2 : 0) + (track.default_track ? 0 : 1)
                         : track.best          ? 8
                         : track.default_track ? 9
                                               : 10;
        if (best < 0 || rank < best_rank)
        {
            best = static_cast<int>(i);
            best_rank = rank;
        }
    }
    return std::max(best, 0);
}

std::vector<int> main_genres(const Movie &movie)
{
    std::vector<int> out;
    for (const int genre : movie.genres)
        if (out.size() < 2 && !genre_name(genre).empty() &&
            std::find(out.begin(), out.end(), genre) == out.end())
            out.push_back(genre);
    return out;
}

std::vector<GenreShelf> genre_shelves(const std::vector<Movie> &movies)
{
    std::map<int, std::vector<int>> shelves;
    for (std::size_t i = 0; i < movies.size(); ++i)
        for (const int genre : main_genres(movies[i]))
            shelves[genre].push_back(static_cast<int>(i));
    std::vector<GenreShelf> out;
    for (auto &entry : shelves)
        out.push_back({entry.first, std::move(entry.second)});
    std::stable_sort(out.begin(), out.end(), [](const GenreShelf &a, const GenreShelf &b)
                     { return a.movies.size() > b.movies.size(); });
    return out;
}

bool matches_search(const Movie &movie, std::string_view query)
{
    const std::string wanted = search_fold(query);
    if (wanted.empty())
        return true;
    // TMDB's title and the file's: a film is found by either, so the words of a name the
    // viewer gave the file still find it after TMDB named it in another language.
    std::string haystack = search_fold(movie.title);
    if (!movie.file_title.empty() && movie.file_title != movie.title)
        haystack += " " + search_fold(movie.file_title);
    for (const std::string_view word : split(wanted, ' '))
        if (!word.empty() && haystack.find(word) == std::string::npos)
            return false;
    return true;
}

Library Library::from_scan(const ScanResult &scan, const RootContents &contents)
{
    Library library;
    library.roots_ = scan.roots;

    // Sidecar files grouped by folder, so pairing never rescans the whole list.
    std::map<std::string, std::vector<const FoundFile *>> images;
    std::map<std::string, std::vector<const FoundFile *>> subtitles;
    std::map<std::string, int> videos_per_folder;
    for (const FoundFile &file : scan.images)
        images[directory_of(file.path)].push_back(&file);
    for (const FoundFile &file : scan.subtitles)
        subtitles[lower(directory_of(file.path))].push_back(&file);
    for (const FoundFile &file : scan.videos)
        ++videos_per_folder[directory_of(file.path)];

    // The root a file is under: the deepest one, as an extra folder may sit inside
    // another ("/mnt/usb0" and "/mnt/usb0/movies"), and the collection is named from it.
    const auto root_of = [&](const std::string &path) -> const std::string *
    {
        const std::string *found = nullptr;
        for (const std::string &root : scan.roots)
            if (path.compare(0, root.size() + 1, root + "/") == 0 &&
                (found == nullptr || root.size() > found->size()))
                found = &root;
        return found;
    };
    // The series folder a file is under, empty if none: the root itself when it is named like
    // one, else the first such folder below it. Extra folders are roots of any name, and the
    // viewer files series in them as on a drive ("Media/Serien/Show/Staffel 1/05.mkv"). The
    // first one wins, so a series root keeps a series named "Shows" as a series.
    const auto series_root_of = [&](const std::string &path, const std::string *root)
    {
        if (root == nullptr || is_series_folder(file_name_of(*root)))
            return root != nullptr ? *root : std::string();
        std::string folder = *root;
        const std::string rest = path.substr(root->size() + 1);
        std::vector<std::string_view> parts = split(rest, '/');
        parts.pop_back(); // the file itself
        for (const std::string_view part : parts)
        {
            folder += "/";
            folder += part;
            if (is_series_folder(part))
                return folder;
        }
        return std::string();
    };
    // Subtitles sit beside the video or in a "subs" folder next to it. Beside the video they
    // must carry its name; in "subs" a video that is alone in its folder owns everything there
    // ("2_English.srt").
    const auto subtitles_of = [&](const std::string &folder, const std::string &base, bool alone)
    {
        std::vector<SubtitleFile> found;
        const std::string key = lower(base);
        std::size_t english_found = 0; // English tracks sit first, in the order they were found
        for (const char *where : {"", "/subs", "/sub", "/subtitles"})
        {
            const auto it = subtitles.find(lower(folder) + where);
            if (it == subtitles.end())
                continue;
            const bool own_folder = where[0] == '\0';
            for (const FoundFile *subtitle : it->second)
            {
                const std::string sub = lower(base_name(subtitle->path));
                if (sub.compare(0, key.size(), key) != 0 && (own_folder || !alone))
                    continue;
                if (found.size() >= kMaxSubtitles)
                    break;
                const bool english = sub.find(".en") != std::string::npos ||
                                     sub.find("english") != std::string::npos ||
                                     sub.find("eng") != std::string::npos;
                const std::size_t slash = subtitle->path.rfind('/');
                const std::string code = language_from_filename(
                    slash == std::string::npos ? subtitle->path : subtitle->path.substr(slash + 1));
                SubtitleFile track{subtitle->readable, code.empty() ? "und" : code};
                const auto at = found.begin() +
                                static_cast<std::ptrdiff_t>(english ? english_found : found.size());
                found.insert(at, std::move(track));
                english_found += english ? 1 : 0;
            }
        }
        return found;
    };

    // The series, by the key of their name: episodes of one series on two drives meet.
    struct ShowBuild
    {
        Movie show;
        std::map<int, Season> seasons;
    };
    std::map<std::string, ShowBuild> shows;
    // Files an episode in its series; false when it has no series to go into (a lone
    // "S01E02.mkv" straight in a movies folder), and it is shown as a film.
    // `series_root`: below `root` every folder is a series ("tvshows", or one chosen so).
    const auto add_episode = [&](const FoundFile &file, const std::string &id,
                                 const EpisodeName &name, const std::string *root,
                                 bool series_root) -> bool
    {
        // The folders between the root and the file: "Show Name", "Season 1".
        std::vector<std::string> folders;
        {
            const std::string rest = root != nullptr ? file.path.substr(root->size() + 1)
                                                     : file_name_of(directory_of(file.path)) + "/" +
                                                           file_name_of(file.path);
            for (const std::string_view part : split(rest, '/'))
                folders.emplace_back(part);
            folders.pop_back(); // the file itself
        }
        series_root = series_root && root != nullptr;
        // The series' folder: below a series root the first one ("tvshows/Show/Season 1/x"),
        // elsewhere the file's own, or the one above it when that is a season.
        int show_at = -1;
        if (!folders.empty())
        {
            show_at = series_root ? 0 : static_cast<int>(folders.size()) - 1;
            if (!series_root && season_of_folder(folders.back()) >= 0)
                --show_at;
        }
        int folder_season = -1;
        for (int i = show_at + 1; i < static_cast<int>(folders.size()); ++i)
            if (const int season = season_of_folder(folders[static_cast<std::size_t>(i)]);
                season >= 0)
                folder_season = season;

        ParsedName show_name;
        std::string show_folder;
        if (show_at >= 0)
        {
            show_name = parse_file_name(folders[static_cast<std::size_t>(show_at)] + ".mkv");
            show_folder = root != nullptr ? *root : directory_of(directory_of(file.path));
            for (int i = 0; i <= show_at; ++i)
                show_folder += "/" + folders[static_cast<std::size_t>(i)];
        }
        else if (!name.show.empty())
        {
            show_name = parse_file_name(name.show + ".mkv");
        }
        if (show_name.title.empty())
            return false;

        // The key ignores case, punctuation and a leading article: "The Show" and "the.show" meet.
        std::string key = sort_key(show_name.title);
        if (key.empty())
            key = lower(show_name.title);
        ShowBuild &build = shows[key];
        Movie &show = build.show;
        if (show.id.empty())
        {
            char hash[24];
            std::snprintf(hash, sizeof(hash), "tv%08x", save::crc32("show:" + key));
            show.id = hash;
            show.show = true;
            show.title = show_name.title;
            show.file_title = show_name.title;
            show.path = show_folder.empty() ? directory_of(file.path) : show_folder;
            show.drive = drive_label(file.path);
        }
        if (show.year == 0)
            show.year = show_name.year;
        if (show.poster.empty() || show.backdrop.empty())
            if (const auto it = images.find(show_folder);
                !show_folder.empty() && it != images.end())
            {
                const std::string base = file_name_of(show_folder);
                if (show.poster.empty())
                    if (const FoundFile *poster =
                            pick(it->second, base, {"", "-poster", ".poster", "-cover"},
                                 {"poster", "folder", "cover", "show"}, true))
                        show.poster = poster->readable;
                if (show.backdrop.empty())
                    if (const FoundFile *backdrop =
                            pick(it->second, base, {"-fanart", "-backdrop", "-background"},
                                 {"fanart", "backdrop", "background", "landscape"}, true))
                        show.backdrop = backdrop->readable;
            }

        Episode episode;
        episode.id = id;
        episode.path = file.path;
        episode.season = name.marked ? name.season : std::max(folder_season, 1);
        if (!name.marked && folder_season == 0)
            episode.season = 0;
        episode.number = name.episode;
        episode.file_title = name.title;
        episode.title = name.title;
        episode.quality = parse_file_name(file_name_of(file.path)).quality;
        episode.size = file.size;
        episode.added = file.mtime;
        episode.drive = drive_label(file.path);
        const std::string folder = directory_of(file.path);
        episode.subtitles =
            subtitles_of(folder, base_name(file.path), videos_per_folder[folder] == 1);

        show.size += file.size;
        show.added = std::max(show.added, file.mtime);
        if (quality_rank(episode.quality) > quality_rank(show.quality))
            show.quality = episode.quality;
        Season &season = build.seasons[episode.season];
        season.number = episode.season;
        season.episodes.push_back(std::move(episode));
        return true;
    };

    std::set<std::string> seen; // the same file reachable through two roots
    std::map<std::string, int> collection_sizes;
    for (const FoundFile &file : scan.videos)
    {
        const std::string name = file_name_of(file.path);
        const std::string id = make_id(name, file.size);
        if (!seen.insert(id).second)
            continue;

        const std::string *root = root_of(file.path);
        const EpisodeName episode = parse_episode_name(name);
        FolderContent content = FolderContent::automatic;
        if (const auto chosen = root != nullptr ? contents.find(*root) : contents.end();
            chosen != contents.end())
            content = chosen->second;
        const std::string series_root = content == FolderContent::series ? *root
                                        : content == FolderContent::automatic
                                            ? series_root_of(file.path, root)
                                            : std::string();
        if (content != FolderContent::movies && (episode.marked || !series_root.empty()) &&
            add_episode(file, id, episode, series_root.empty() ? root : &series_root,
                        !series_root.empty()))
            continue;

        Movie movie;
        movie.id = id;
        movie.path = file.path;
        movie.size = file.size;
        movie.added = file.mtime;
        movie.drive = drive_label(file.path);

        const std::string folder = directory_of(file.path);
        ParsedName parsed = parse_file_name(name);
        const ParsedName from_folder = parse_file_name(file_name_of(folder));
        // "Name (2010)/video.mkv": the folder knows more than the file.
        if (is_generic_name(base_name(name)) ||
            (parsed.year == 0 && from_folder.year != 0 && videos_per_folder[folder] == 1))
        {
            parsed.title = from_folder.title.empty() ? parsed.title : from_folder.title;
            if (from_folder.year != 0)
                parsed.year = from_folder.year;
        }
        movie.title = parsed.title;
        movie.file_title = parsed.title;
        movie.year = parsed.year;
        movie.quality = parsed.quality;

        const bool alone = videos_per_folder[folder] == 1;
        const std::string base = base_name(name);
        if (const auto it = images.find(folder); it != images.end())
        {
            if (const FoundFile *poster =
                    pick(it->second, base, {"", "-poster", ".poster", "-thumb", "-cover"},
                         {"poster", "folder", "cover", "movie"}, alone))
                movie.poster = poster->readable;
            if (const FoundFile *backdrop =
                    pick(it->second, base, {"-fanart", "-backdrop", "-landscape", "-background"},
                         {"fanart", "backdrop", "background", "landscape"}, alone))
                movie.backdrop = backdrop->readable;
        }
        movie.subtitles = subtitles_of(folder, base, alone);

        // The first folder under a movies root names a collection, if it is
        // shared: "movies/Space Sagas/Part 1.mkv" and "…/Part 2.mkv".
        if (root != nullptr)
        {
            const std::string rest = file.path.substr(root->size() + 1);
            const std::size_t slash = rest.find('/');
            if (slash != std::string::npos)
            {
                movie.collection = rest.substr(0, slash);
                ++collection_sizes[*root + "/" + movie.collection];
            }
        }
        library.movies_.push_back(std::move(movie));
    }

    // A folder holding a single film is that film's own folder, not a collection.
    for (Movie &movie : library.movies_)
    {
        if (movie.collection.empty())
            continue;
        if (const std::string *root = root_of(movie.path);
            root != nullptr && collection_sizes[*root + "/" + movie.collection] < 2)
            movie.collection.clear();
    }

    for (auto &entry : shows)
    {
        Movie &show = entry.second.show;
        for (auto &numbered : entry.second.seasons)
        {
            Season &season = numbered.second;
            std::stable_sort(season.episodes.begin(), season.episodes.end(),
                             [](const Episode &a, const Episode &b)
                             {
                                 // Numbered first, in order; the rest by their file names.
                                 if ((a.number == 0) != (b.number == 0))
                                     return a.number != 0;
                                 if (a.number != b.number)
                                     return a.number < b.number;
                                 return lower(file_name_of(a.path)) < lower(file_name_of(b.path));
                             });
            // A season whose files carry no numbers is counted in the order of their names.
            if (std::all_of(season.episodes.begin(), season.episodes.end(),
                            [](const Episode &e) { return e.number == 0; }))
                for (std::size_t i = 0; i < season.episodes.size(); ++i)
                    season.episodes[i].number = static_cast<int>(i) + 1;
            show.seasons.push_back(std::move(season));
        }
        // std::map orders the seasons 0, 1, 2 ...: the specials go to the end.
        if (show.seasons.size() > 1 && show.seasons.front().number == 0)
            std::rotate(show.seasons.begin(), show.seasons.begin() + 1, show.seasons.end());
        library.movies_.push_back(std::move(show));
    }

    library.sort();
    return library;
}

int Library::find_episode(std::string_view id, EpisodeRef *ref) const
{
    for (std::size_t i = 0; i < movies_.size(); ++i)
    {
        if (!movies_[i].show)
            continue;
        const EpisodeRef found = media::find_episode(movies_[i], id);
        if (found.valid())
        {
            if (ref != nullptr)
                *ref = found;
            return static_cast<int>(i);
        }
    }
    if (ref != nullptr)
        *ref = EpisodeRef{};
    return -1;
}

std::size_t Library::film_count() const
{
    return static_cast<std::size_t>(
        std::count_if(movies_.begin(), movies_.end(), [](const Movie &m) { return !m.show; }));
}

std::size_t Library::show_count() const
{
    return movies_.size() - film_count();
}

const Episode *episode_at(const Movie &show, EpisodeRef ref)
{
    if (!ref.valid() || static_cast<std::size_t>(ref.season) >= show.seasons.size())
        return nullptr;
    const Season &season = show.seasons[static_cast<std::size_t>(ref.season)];
    if (static_cast<std::size_t>(ref.episode) >= season.episodes.size())
        return nullptr;
    return &season.episodes[static_cast<std::size_t>(ref.episode)];
}

EpisodeRef find_episode(const Movie &show, std::string_view id)
{
    for (std::size_t s = 0; s < show.seasons.size(); ++s)
        for (std::size_t e = 0; e < show.seasons[s].episodes.size(); ++e)
            if (show.seasons[s].episodes[e].id == id)
                return {static_cast<int>(s), static_cast<int>(e)};
    return {};
}

EpisodeRef first_episode(const Movie &show)
{
    // The specials are last (from_scan), so the first season with episodes is a numbered one
    // whenever there is one.
    for (std::size_t s = 0; s < show.seasons.size(); ++s)
        if (!show.seasons[s].episodes.empty())
            return {static_cast<int>(s), 0};
    return {};
}

EpisodeRef next_episode(const Movie &show, EpisodeRef ref)
{
    if (episode_at(show, ref) == nullptr)
        return {};
    if (static_cast<std::size_t>(ref.episode) + 1 <
        show.seasons[static_cast<std::size_t>(ref.season)].episodes.size())
        return {ref.season, ref.episode + 1};
    for (std::size_t s = static_cast<std::size_t>(ref.season) + 1; s < show.seasons.size(); ++s)
        if (!show.seasons[s].episodes.empty() && show.seasons[s].number != 0)
            return {static_cast<int>(s), 0};
    return {};
}

std::size_t episode_count(const Movie &show)
{
    std::size_t count = 0;
    for (const Season &season : show.seasons)
        count += season.episodes.size();
    return count;
}

std::size_t season_total(const Movie &show)
{
    const std::size_t numbered = static_cast<std::size_t>(std::count_if(
        show.seasons.begin(), show.seasons.end(), [](const Season &s) { return s.number > 0; }));
    return numbered > 0 ? numbered : show.seasons.size();
}

bool is_series_folder(std::string_view folder_name)
{
    const std::string key = lower(folder_name);
    return key == "tvshows" || key == "tv shows" || key == "tv-shows" || key == "tv_shows" ||
           key == "series" || key == "serien" || key == "shows";
}

void Library::sort()
{
    std::stable_sort(movies_.begin(), movies_.end(),
                     [](const Movie &a, const Movie &b)
                     {
                         const std::string ka = sort_key(a.title);
                         const std::string kb = sort_key(b.title);
                         return ka != kb ? ka < kb : a.year < b.year;
                     });
}

void Library::edit_all(const std::function<void(Movie *)> &edit)
{
    for (Movie &movie : movies_)
        edit(&movie);
}

int Library::find(std::string_view id) const
{
    for (std::size_t i = 0; i < movies_.size(); ++i)
        if (movies_[i].id == id)
            return static_cast<int>(i);
    return -1;
}

} // namespace hui::media
