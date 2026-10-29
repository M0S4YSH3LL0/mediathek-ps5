#!/usr/bin/env python3
# Mediathek - Downloads a poster (and optionally a backdrop) for every movie that lacks one.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/fetch-posters.py [--backdrops] [--dry-run] [--force] FOLDER...
#
# Run it on a PC with the movie drive plugged in (or the folder mounted), then
# press Options on the console to rescan. The pictures are saved next to each
# film as "<film name>-poster.jpg" and "<film name>-fanart.jpg", which is where
# the app looks for them; a film that already has a poster is left alone.
#
# The data comes from The Movie Database (themoviedb.org), which gives a free
# key to personal projects: https://www.themoviedb.org/settings/api . Put the
# key in TMDB_API_KEY (the 32-character "API Key" or the long "Read Access
# Token") in the environment or in the project's .env file, or pass --key.
# IMDb has no free poster service. The console app fetches covers by itself
# from the same .env (see README); this tool does the same job from a PC.
#
# This product uses the TMDB API but is not endorsed or certified by TMDB.

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# The environment overrides let a test point the tool at a local fake server.
API = os.environ.get("TMDB_API_URL", "https://api.themoviedb.org/3")
IMAGES = os.environ.get("TMDB_IMAGE_URL", "https://image.tmdb.org/t/p")
POSTER_WIDTH = "w500"  # the app draws posters about 300 px wide at 1080p
BACKDROP_WIDTH = "w1280"
PAUSE = float(os.environ.get("TMDB_PAUSE", "0.12"))  # seconds between requests, well under the service's limit

VIDEO = {"mkv", "mp4", "m4v", "avi", "mov", "ts", "m2ts", "webm", "wmv", "mpg", "mpeg", "mts"}
IMAGE = {"jpg", "jpeg", "png"}
# Mirrors src/media/title.cpp: words that start the release-tag tail.
TAGS = {
    "2160p", "1080p", "1080i", "720p", "576p", "480p", "4k", "uhd", "bluray", "blu-ray", "bdrip",
    "brrip", "webrip", "web-dl", "webdl", "hdtv", "dvdrip", "dvd", "hdrip", "remux", "x264",
    "x265", "h264", "h265", "hevc", "avc", "xvid", "divx", "hdr", "hdr10", "dv", "dts", "ac3",
    "aac", "atmos", "truehd", "proper", "repack", "extended", "imax",
}  # fmt: skip
GENERIC = {"movie", "video", "main", "film", "title"}
# Mirrors pick() in src/media/library.cpp.
POSTER_SUFFIXES = ("", "-poster", ".poster", "-thumb", "-cover")
POSTER_BARE = ("poster", "folder", "cover", "movie")
BACKDROP_SUFFIXES = ("-fanart", "-backdrop", "-landscape", "-background")
BACKDROP_BARE = ("fanart", "backdrop", "background", "landscape")


def read_dotenv():
    """TMDB_API_KEY and TMDB_LANGUAGE from the project's .env, if it has them."""
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".env")
    values = {}
    try:
        with open(path, encoding="utf-8") as source:
            for line in source:
                line = line.strip()
                if line.startswith("export "):
                    line = line[7:].strip()
                name, equals, value = line.partition("=")
                if equals and not name.startswith("#"):
                    values[name.strip()] = value.split(" #")[0].strip().strip("\"'")
    except OSError:
        pass
    return values


def split_ext(name):
    stem, dot, ext = name.rpartition(".")
    if not dot or not stem or len(ext) > 5:
        return name, ""
    return stem, ext.lower()


def parse_name(name):
    """(title, year) from a file or folder name; year is 0 when there is none."""
    stem, ext = split_ext(name)
    if ext not in VIDEO and ext not in IMAGE:
        stem = name
    words = [w for w in re.split(r"[._ ()\[\]{}]+", stem) if w]
    end = len(words)
    for i, word in enumerate(words):
        if word.lower() in TAGS:
            end = min(end, i)
    year = 0
    for i in range(end):
        if i > 0 and re.fullmatch(r"(19|20)\d\d|2100", words[i]):
            year = int(words[i])
            end = i
    if end == 0 and words:
        end = 1
    return " ".join(words[:end]), year


class Movie:
    def __init__(self, path, title, year):
        self.path = path
        self.folder = os.path.dirname(path)
        self.base = split_ext(os.path.basename(path))[0]
        self.title = title
        self.year = year


def find_movies(roots):
    for root in roots:
        for folder, dirs, files in os.walk(root):
            dirs[:] = sorted(d for d in dirs if not d.startswith("."))
            videos = [f for f in sorted(files) if not f.startswith(".") and split_ext(f)[1] in VIDEO]
            for name in videos:
                title, year = parse_name(name)
                from_folder = parse_name(os.path.basename(folder))
                # "Name (2010)/video.mkv": the folder knows more than the file.
                if split_ext(name)[0].lower() in GENERIC or len(split_ext(name)[0]) < 3 or (
                    year == 0 and from_folder[1] and len(videos) == 1
                ):
                    title = from_folder[0] or title
                    year = from_folder[1] or year
                yield Movie(os.path.join(folder, name), title, year), len(videos) == 1


def has_picture(movie, alone, suffixes, bare, files):
    names = {split_ext(f)[0].lower() for f in files if split_ext(f)[1] in IMAGE}
    if any(movie.base.lower() + s in names for s in suffixes):
        return True
    return alone and any(word in names for word in bare)


class Tmdb:
    def __init__(self, key, language):
        self.key = key
        self.language = language

    def get(self, url, binary=False):
        request = urllib.request.Request(url, headers={"User-Agent": "mediathek-fetch-posters/1"})
        if not binary and len(self.key) > 40:  # a v4 read access token
            request.add_header("Authorization", "Bearer " + self.key)
        time.sleep(PAUSE)
        for attempt in range(3):
            try:
                with urllib.request.urlopen(request, timeout=30) as response:
                    data = response.read()
                return data if binary else json.loads(data)
            except urllib.error.HTTPError as error:
                if error.code == 429 and attempt < 2:  # asked to slow down
                    time.sleep(float(error.headers.get("Retry-After", "2")))
                    continue
                raise
        raise RuntimeError("unreachable")

    def search(self, title, year):
        params = {"query": title, "language": self.language, "include_adult": "false"}
        if year:
            params["year"] = str(year)
        if len(self.key) <= 40:
            params["api_key"] = self.key
        found = self.get(API + "/search/movie?" + urllib.parse.urlencode(params))["results"]
        if not found and year:  # a wrong year in the file name should not hide the film
            return self.search(title, 0)
        # The best match has a picture; among those the service's own order stands,
        # except that an exact year wins over a near one.
        with_poster = [r for r in found if r.get("poster_path")]
        for result in with_poster:
            if year and result.get("release_date", "").startswith(str(year)):
                return result
        return with_poster[0] if with_poster else (found[0] if found else None)

    def image(self, path, width):
        return self.get(IMAGES + "/" + width + path, binary=True)


def save(path, data):
    # An error page saved as "poster.jpg" would sit there for good.
    if not (data.startswith(b"\xff\xd8") or data.startswith(b"\x89PNG")):
        raise ValueError("the download is not a picture")
    part = path + ".part"
    with open(part, "wb") as out:
        out.write(data)
    os.replace(part, path)


def main():
    parser = argparse.ArgumentParser(description="Download movie posters from TMDb.")
    parser.add_argument("folders", nargs="+", help="movie folders, e.g. /media/me/DRIVE/movies")
    dotenv = read_dotenv()
    parser.add_argument("--key", help="TMDb key (default: TMDB_API_KEY, or .env)")
    parser.add_argument("--language", help="poster language, e.g. de-DE (default: TMDB_LANGUAGE or en-US)")
    parser.add_argument("--backdrops", action="store_true", help="also fetch the wide picture")
    parser.add_argument("--force", action="store_true", help="replace pictures that exist")
    parser.add_argument("--dry-run", action="store_true", help="only list what is missing")
    args = parser.parse_args()
    args.key = args.key or os.environ.get("TMDB_API_KEY") or dotenv.get("TMDB_API_KEY", "")
    args.language = args.language or os.environ.get("TMDB_LANGUAGE") or dotenv.get(
        "TMDB_LANGUAGE", "en-US"
    )

    if not args.key and not args.dry_run:
        sys.exit("No TMDb key. Get a free one at https://www.themoviedb.org/settings/api\n"
                 "and put TMDB_API_KEY=<key> in the project's .env file")
    tmdb = Tmdb(args.key, args.language)
    cache = {}
    done = missing = failed = 0
    for movie, alone in find_movies(args.folders):
        files = os.listdir(movie.folder)
        want_poster = args.force or not has_picture(movie, alone, POSTER_SUFFIXES, POSTER_BARE, files)
        want_backdrop = args.backdrops and (
            args.force or not has_picture(movie, alone, BACKDROP_SUFFIXES, BACKDROP_BARE, files)
        )
        if not (want_poster or want_backdrop):
            continue
        label = movie.title + (" (%d)" % movie.year if movie.year else "")
        if args.dry_run:
            print("missing: %s  <- %s" % (label, os.path.basename(movie.path)))
            missing += 1
            continue
        try:
            key = (movie.title.lower(), movie.year)
            if key not in cache:
                cache[key] = tmdb.search(movie.title, movie.year)
            result = cache[key]
            if not result:
                print("no match:  %s" % label)
                failed += 1
                continue
            stem = os.path.join(movie.folder, movie.base)
            if want_poster and result.get("poster_path"):
                save(stem + "-poster.jpg", tmdb.image(result["poster_path"], POSTER_WIDTH))
            if want_backdrop and result.get("backdrop_path"):
                save(stem + "-fanart.jpg", tmdb.image(result["backdrop_path"], BACKDROP_WIDTH))
            print("ok:        %s  ->  %s" % (label, result.get("title", "")))
            done += 1
        except (urllib.error.URLError, OSError, ValueError) as error:
            print("failed:    %s (%s)" % (label, error))
            failed += 1
    print("\n%d fetched, %d without a match or failed, %d listed" % (done, failed, missing))
    if done:
        print("Press Options in Mediathek to rescan.")
    print("This product uses the TMDB API but is not endorsed or certified by TMDB.")
    return 1 if failed and not done else 0


if __name__ == "__main__":
    sys.exit(main())
