// Mediathek - Where the viewer is in a series: the episode up next and whether it is under way.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/library.hpp"
#include "media/progress.hpp"

#include <cstdint>

namespace hui::media
{

struct ShowProgress
{
    // The episode Play starts: the one last played if it is under way, else the first one after
    // it not yet seen, else (nothing played, or everything seen) the first one.
    EpisodeRef next;
    // When an episode of the series was last played; 0 for never. Orders Continue Watching.
    std::int64_t last = 0;
    // The series belongs on Continue Watching: an episode is under way, or one was finished and
    // there is one after it still to see.
    bool continuing = false;
    bool all_watched = false; // every episode is marked watched
};

ShowProgress show_progress(const Movie &show, const ProgressStore &progress);

} // namespace hui::media
