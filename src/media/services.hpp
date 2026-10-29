// Mediathek - What a screen needs from the media side: library, art, resume points, player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/info_service.hpp"
#include "media/library.hpp"
#include "media/player.hpp"
#include "media/poster_cache.hpp"
#include "media/progress.hpp"

#include <cstdint>
#include <functional>
#include <string>

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
    // Fetches this movie's cover and wallpaper again. Null without a cover service.
    // Returns "" once started, else a sentence for the screen about why not.
    std::function<std::string(const std::string &id)> refetch_cover;
    // What stops the covers coming ("No TMDB_API_KEY in mediathek.env"), or "".
    std::string cover_note;
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
