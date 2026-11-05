// Mediathek - What a screen needs from the media side: library, art, resume points, player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/i18n.hpp"
#include "media/info_service.hpp"
#include "media/library.hpp"
#include "media/player.hpp"
#include "media/poster_cache.hpp"
#include "media/progress.hpp"
#include "media/scan_folders.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hui::media
{

// Owned by the platform layer (main.cpp, the snapshot tool, the tests); the
// screen only borrows it through app::Context.
struct Services
{
    Library library;
    ProgressStore progress;
    PosterCache *posters = nullptr;
    Player *player = nullptr;
    // The screen goes out as HDR10 now: the platform draws the playing film's picture itself
    // (app::Frame::video) and the screen leaves its place clear. Set by the platform.
    bool hdr_output = false;
    // Reads what a file contains (codecs, length, languages) for the details page.
    // Null where nothing can read a file; the page then shows what the names say.
    InfoService *info = nullptr;

    // Bumped whenever `library` is replaced, so a screen rebuilds its shelves.
    std::uint32_t revision = 1;
    // A scan is under way (the helper payload is listing the drives).
    bool scanning = false;
    // Asks the platform to close the app. Null where closing makes no sense.
    std::function<void()> quit;
    // Asks the platform to scan again. Null when scanning is not possible.
    std::function<void()> rescan;
    // The folders scanned besides the default ones (Settings > Library), as
    // clean_scan_folder() leaves them: a path, or a name looked for on every drive, and what
    // each holds.
    std::vector<ScanFolder> scan_folders;
    // Keeps `scan_folders` after the screen changed them and scans again (after the scan
    // that is running, if one is). Null where they cannot be kept.
    std::function<void()> scan_folders_changed;
    // Fetches this movie's cover and wallpaper again. Null without a cover service.
    // Returns "" once started, else a sentence for the screen about why not.
    std::function<std::string(const std::string &id)> refetch_cover;
    // What stops the covers coming ("No TMDB_API_KEY in mediathek.env"), or "".
    std::string cover_note;
    // Cover jobs not yet finished, the running one included. The platform sets it every frame;
    // the screen counts a reload's progress with it.
    std::size_t cover_jobs = 0;
    // The language "automatic" means in Settings: the console's, or UI_LANGUAGE's.
    i18n::Language automatic_language = i18n::Language::english;
    // Set by the platform when a scan finished: how many movies it found, -1 when there is
    // nothing to announce. The screen says it (if the viewer wants debug messages) and resets it.
    int scan_found = -1;
    // One line about the scan for the empty screen: "Waiting for the ELF loader".
    std::string scan_note;
    // Wall-clock seconds since the epoch; resume points are stamped with it.
    std::function<std::int64_t()> clock;

    std::int64_t now() const
    {
        return clock ? clock() : 0;
    }
};

} // namespace hui::media
