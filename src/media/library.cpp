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

std::vector<std::string> default_roots()
{
    // The same drives and folders, in the same order, as main() in helper/mt-scan.c.
    std::vector<std::string> drives = {"/data", ""};
    for (int i = 0; i < 8; ++i)
        drives.push_back("/mnt/usb" + std::to_string(i));
    for (int i = 0; i < 2; ++i)
        drives.push_back("/mnt/ext" + std::to_string(i));
    std::vector<std::string> roots;
    for (const std::string &drive : drives)
        for (const char *folder : {"movies", "Movies", "mediathek/movies", "mediathek/Movies"})
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
    const std::string haystack = search_fold(movie.title);
    for (const std::string_view word : split(wanted, ' '))
        if (!word.empty() && haystack.find(word) == std::string::npos)
            return false;
    return true;
}

Library Library::from_scan(const ScanResult &scan)
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

    std::set<std::string> seen; // the same file reachable through two roots
    std::map<std::string, int> collection_sizes;
    for (const FoundFile &file : scan.videos)
    {
        const std::string name = file_name_of(file.path);
        const std::string id = make_id(name, file.size);
        if (!seen.insert(id).second)
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
        // Subtitles sit beside the film or in a "subs" folder next to it. Beside
        // the film they must carry its name; in "subs" a film that is alone in
        // its folder owns everything there ("2_English.srt").
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
                if (movie.subtitles.size() >= kMaxSubtitles)
                    break;
                const bool english = sub.find(".en") != std::string::npos ||
                                     sub.find("english") != std::string::npos ||
                                     sub.find("eng") != std::string::npos;
                const std::size_t slash = subtitle->path.rfind('/');
                const std::string code = language_from_filename(
                    slash == std::string::npos ? subtitle->path : subtitle->path.substr(slash + 1));
                SubtitleFile track{subtitle->readable, code.empty() ? "und" : code};
                const auto at =
                    movie.subtitles.begin() +
                    static_cast<std::ptrdiff_t>(english ? english_found : movie.subtitles.size());
                movie.subtitles.insert(at, std::move(track));
                english_found += english ? 1 : 0;
            }
        }

        // The first folder under a movies root names a collection, if it is
        // shared: "movies/Space Sagas/Part 1.mkv" and "…/Part 2.mkv".
        for (const std::string &root : scan.roots)
        {
            if (file.path.compare(0, root.size() + 1, root + "/") != 0)
                continue;
            const std::string rest = file.path.substr(root.size() + 1);
            const std::size_t slash = rest.find('/');
            if (slash != std::string::npos)
            {
                movie.collection = rest.substr(0, slash);
                ++collection_sizes[root + "/" + movie.collection];
            }
            break;
        }
        library.movies_.push_back(std::move(movie));
    }

    // A folder holding a single film is that film's own folder, not a collection.
    for (Movie &movie : library.movies_)
    {
        if (movie.collection.empty())
            continue;
        for (const std::string &root : scan.roots)
        {
            if (movie.path.compare(0, root.size() + 1, root + "/") != 0)
                continue;
            if (collection_sizes[root + "/" + movie.collection] < 2)
                movie.collection.clear();
            break;
        }
    }

    std::stable_sort(library.movies_.begin(), library.movies_.end(),
                     [](const Movie &a, const Movie &b)
                     {
                         const std::string ka = sort_key(a.title);
                         const std::string kb = sort_key(b.title);
                         return ka != kb ? ka < kb : a.year < b.year;
                     });
    return library;
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
