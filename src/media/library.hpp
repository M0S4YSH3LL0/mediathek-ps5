// Mediathek - The movie library: files found on the console's drives and their artwork.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
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
    bool forced = false; // its name says so (forced_from_filename)
};

// One file of a series. Its id is made like a film's, so resume points, the file's details and
// the fetched description are kept under it the same way.
struct Episode
{
    std::string id;
    std::string path;
    int season = 1;         // 0: the specials
    int number = 0;         // within the season; 0 when the name gives none
    std::string title;      // TMDB's name for it, else file_title (may be "")
    std::string file_title; // what the file name says after the episode mark, or ""
    std::string overview;   // TMDB's description, or ""
    std::string quality;
    std::uint64_t size = 0;
    std::int64_t added = 0; // as Movie::added
    std::string drive;
    std::vector<SubtitleFile> subtitles;
};

struct Season
{
    int number = 1;
    std::string overview;          // TMDB's description of the season, or ""
    std::vector<Episode> episodes; // by number, then by file name
};

// A film, or a whole series (`show`): the shelves, the grid and the details page show both
// the same way. A series has no file of its own; its episodes are in `seasons`, its `path` is
// its folder and its id is "tv" and a hash of its name, so it never meets a film's.
struct Movie
{
    std::string id;    // stable across drive letters: name and size, hashed
    std::string path;  // the video file
    std::string title; // what is shown: TMDB's title once fetched, else file_title
    // "The Long Walk Home": the title the file name gives, which drops the punctuation a
    // file name cannot carry. TMDB is searched with it, and it is the title while TMDB
    // has none.
    std::string file_title;
    int year = 0;        // 0 = unknown
    std::string quality; // "4K", "1080p", "720p", "SD" or ""
    std::uint64_t size = 0;
    std::int64_t added = 0; // first seen (AddedStore), else modification time; epoch seconds
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
    bool show = false;
    std::vector<Season> seasons; // a series': numbered seasons first, the specials last
};

// Where an episode is in its series: indices into seasons and episodes, -1 for none.
struct EpisodeRef
{
    int season = -1;
    int episode = -1;
    bool valid() const
    {
        return season >= 0 && episode >= 0;
    }
    bool operator==(const EpisodeRef &) const = default;
};

// The episode a reference points at, or null.
const Episode *episode_at(const Movie &show, EpisodeRef ref);
// The episode with this id in the series, or an invalid reference.
EpisodeRef find_episode(const Movie &show, std::string_view id);
// The first episode: of the first numbered season, the specials only when there is nothing else.
EpisodeRef first_episode(const Movie &show);
// The episode after `ref`, into the next season at the end of one; invalid after the last.
EpisodeRef next_episode(const Movie &show, EpisodeRef ref);
// How many episodes a series has.
std::size_t episode_count(const Movie &show);
// How many seasons a series has, as a viewer counts them: the specials are not one, unless they
// are all there is.
std::size_t season_total(const Movie &show);

// The folders a series is kept in, by their name: "tvshows", "TV Shows", "series", "Serien".
// Every video below one is an episode.
bool is_series_folder(std::string_view folder_name);

// What a folder holds, as the viewer chose it for an extra folder (Settings > Library).
// Automatic goes by the names: a series folder (is_series_folder) anywhere below it holds
// series, and a file with an episode mark is one. Series: every video is an episode, the first
// folder below the root its series, as under "tvshows". Movies: every video is a film.
enum class FolderContent : std::uint8_t
{
    automatic,
    movies,
    series,
};
// The roots whose content was chosen; a root missing here is Automatic.
using RootContents = std::map<std::string, FolderContent>;

// Whether a movie answers a search. `query` is what the viewer typed; every word of it must
// appear in the title, ignoring case and accents. An empty query matches everything.
bool matches_search(const Movie &movie, std::string_view query);

// A film's genres as the app shows them: the first two TMDB lists for it that the app has a
// name for. The details page names them and the Home shelves are made from them, so what the
// page says is what the shelves do. (TMDB lists up to four, in no order of importance; a
// film is not put on every one of them.)
std::vector<int> main_genres(const Movie &movie);

// A subtitle track as the order below sees it: from inside the film or from a file beside it.
struct SubtitleChoice
{
    std::string language;  // canonical ISO 639-2 code, "und" when not known
    bool embedded = false; // a track of the film itself
    bool forced = false;   // only the lines in another language than the film's
};

// The order the subtitle tracks of a film are offered in (indices into `tracks`): those in
// `preferred` (such as "ger") first; within that, the film's own tracks before the .srt files
// beside it, which are more often a poor match for its cut. Otherwise the order given. A forced
// track is not among them: it shows on its own (forced_subtitle).
std::vector<int> subtitle_order(const std::vector<SubtitleChoice> &tracks,
                                std::string_view preferred);

// The forced track to show while no full subtitles are on (an index into `tracks`, -1 for none):
// the first in `language` ("" for none), the film's own before a file.
int forced_subtitle(const std::vector<SubtitleChoice> &tracks, std::string_view language);

// A sound track of a film as the choice below sees it.
struct AudioChoice
{
    std::string language = "und"; // canonical ISO 639-2 code
    bool default_track = false;   // the file marks it as the one to play
    bool extra = false;           // a commentary or an audio description for the blind
    bool best = false;            // FFmpeg's own pick (av_find_best_stream)
};

// The track a film starts with (an index into `tracks`, 0 when there are none). Automatic
// (`preferred` is "") takes the file's choice: FFmpeg's pick, else the track marked default,
// else the first. A language (such as "ger") takes its track, else an English one, else the
// file's choice; within a language a main track before an extra one and the file's default
// before the others.
int preferred_audio_track(const std::vector<AudioChoice> &tracks, std::string_view preferred);

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
//   R<TAB>path                    a movies (or series) folder that exists
//   V<TAB>path<TAB>size<TAB>mtime a video
//   I<TAB>path<TAB>readable       an image next to a video
//   S<TAB>path<TAB>readable       a subtitle file
//   end
// Returns false when the header or the closing "end" line is missing, which
// means the helper was still writing or died.
bool parse_index(std::string_view text, ScanResult *out);

// Walks the folders directly (a PC, or a console that allows listing).
ScanResult walk_folders(const std::vector<std::string> &roots);

// The drives a console has, in order: "/data", "" (the system's root), "/mnt/usb0" ...
std::vector<std::string> scan_drives();
// The folders searched on a console, in order.
std::vector<std::string> default_roots();

// Names the drive a path lives on: "/data/movies/x.mkv" is "Internal",
// "/mnt/usb2/movies/x.mkv" is "USB 3".
std::string drive_label(std::string_view path);

class Library
{
  public:
    // Pairs videos with posters, backdrops and subtitles and sorts by title. A file follows
    // the content of the deepest root it is under.
    static Library from_scan(const ScanResult &scan, const RootContents &contents = {});

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
    // Index of the movie (or series) with this id, or -1.
    int find(std::string_view id) const;
    // The series an episode id belongs to: its index, or -1, and where in it the episode is.
    int find_episode(std::string_view id, EpisodeRef *ref) const;
    // The films and the series of the library, counted apart.
    std::size_t film_count() const;
    std::size_t show_count() const;
    // Lets `edit` change every movie (artwork fetched after the scan).
    void edit_all(const std::function<void(Movie *)> &edit);
    // Orders the films by title again, after `edit_all` gave them TMDB's titles. Indices
    // into the library change; ids do not.
    void sort();

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
