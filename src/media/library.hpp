// Mediathek - The movie library: files found on the console's drives and their artwork.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hui::media
{

// A subtitle file beside a film. `language` is what the original file's name says ("ger" for
// "Film.de.srt"), "und" when the name says none; the copy the app reads has a hashed name, so
// it is taken from the original path when the files are paired.
struct SubtitleFile
{
    std::string file; // the readable copy
    std::string language;
};

struct Movie
{
    std::string id;      // stable across drive letters: name and size, hashed
    std::string path;    // the video file
    std::string title;   // "The Long Walk Home"
    int year = 0;        // 0 = unknown
    std::string quality; // "4K", "1080p", "720p", "SD" or ""
    std::uint64_t size = 0;
    std::int64_t added = 0; // modification time, seconds since the epoch
    std::string poster;     // image file, or ""
    std::string backdrop;   // wide image file, or ""
    // Sidecar .srt / .vtt files, English first, then in the order they were found; the first is
    // the one that shows when the film starts. Empty without a subtitle file.
    std::vector<SubtitleFile> subtitles;
    std::string overview;    // the description (fetched from TMDB), or ""
    float rating = 0.0f;     // TMDB's user score, 0 to 10; 0 = none
    std::vector<int> genres; // TMDB genre ids, most telling first; see genre_name()
    std::string drive;       // "Internal", "USB 1", "Drive 2"
    std::string collection;  // folder name when the movie sits in a subfolder of movies/
};

// Whether a movie answers a search. `query` is what the viewer typed; every word of it must
// appear in the title, ignoring case and accents. An empty query matches everything.
bool matches_search(const Movie &movie, std::string_view query);

// A film's genres as the app shows them: the first two TMDB lists for it that the app has a
// name for. The details page names them and the Home shelves are made from them, so what the
// page says is what the shelves do. (TMDB lists up to four, in no order of importance; a
// film is not put on every one of them.)
std::vector<int> main_genres(const Movie &movie);

// The films of one genre shelf (indices into the movie list).
struct GenreShelf
{
    int genre = 0; // TMDB's number
    std::vector<int> movies;
};

// The genre shelves of a library: one for every genre some film has among its main_genres(),
// fullest first (a tie: the lower genre number), each holding exactly the films that have it.
std::vector<GenreShelf> genre_shelves(const std::vector<Movie> &movies);

// A file the scan saw, before names and artwork are paired up.
struct FoundFile
{
    std::string path;
    std::string readable; // where the app can open it (an art cache copy), or path
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

struct ScanResult
{
    std::vector<std::string> roots; // the movies folders that exist
    std::vector<FoundFile> videos;
    std::vector<FoundFile> images;
    std::vector<FoundFile> subtitles;
};

// The index file that the scan helper payload writes (the app's sandbox
// cannot list folders itself):
//   mediathek-index 1
//   R<TAB>path                    a movies folder that exists
//   V<TAB>path<TAB>size<TAB>mtime a video
//   I<TAB>path<TAB>readable       an image next to a video
//   S<TAB>path<TAB>readable       a subtitle file
//   end
// Returns false when the header or the closing "end" line is missing, which
// means the helper was still writing or died.
bool parse_index(std::string_view text, ScanResult *out);

// Walks the folders directly (a PC, or a console that allows listing).
ScanResult walk_folders(const std::vector<std::string> &roots);

// The folders searched on a console, in order.
std::vector<std::string> default_roots();

// Names the drive a path lives on: "/data/movies/x.mkv" is "Internal",
// "/mnt/usb2/movies/x.mkv" is "USB 3".
std::string drive_label(std::string_view path);

class Library
{
  public:
    // Pairs videos with posters, backdrops and subtitles and sorts by title.
    static Library from_scan(const ScanResult &scan);

    const std::vector<Movie> &movies() const
    {
        return movies_;
    }
    std::size_t size() const
    {
        return movies_.size();
    }
    const Movie &operator[](std::size_t index) const
    {
        return movies_[index];
    }
    const std::vector<std::string> &roots() const
    {
        return roots_;
    }
    // Index of the movie with this id, or -1.
    int find(std::string_view id) const;
    // Lets `edit` change every movie (artwork fetched after the scan).
    void edit_all(const std::function<void(Movie *)> &edit);

  private:
    std::vector<Movie> movies_;
    std::vector<std::string> roots_;
};

// A named horizontal shelf of movies (indices into the library).
struct Row
{
    std::string title;
    std::vector<int> movies;
};

} // namespace hui::media
