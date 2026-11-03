// Mediathek - Where the viewer is in a series: the episode up next and whether it is under way.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/show_progress.hpp"

namespace hui::media
{

ShowProgress show_progress(const Movie &show, const ProgressStore &progress)
{
    ShowProgress out;
    out.next = first_episode(show);
    EpisodeRef latest;
    bool any = false;
    bool all = true;
    for (std::size_t s = 0; s < show.seasons.size(); ++s)
        for (std::size_t e = 0; e < show.seasons[s].episodes.size(); ++e)
        {
            any = true;
            const Watch *watch = progress.find(show.seasons[s].episodes[e].id);
            all = all && watch != nullptr && watch->watched;
            if (watch == nullptr || (!watch->watched && !progress.in_progress(watch->id)))
                continue; // looked at for a few seconds only: not where the viewer is
            if (!latest.valid() || watch->last > out.last)
            {
                latest = {static_cast<int>(s), static_cast<int>(e)};
                out.last = watch->last;
            }
        }
    out.all_watched = any && all;
    if (!latest.valid())
        return out;
    const Episode *played = episode_at(show, latest);
    if (progress.in_progress(played->id))
    {
        out.next = latest;
        out.continuing = true;
        return out;
    }
    // Finished: the next one not yet seen, so a series watched out of order does not offer an
    // episode again.
    for (EpisodeRef ref = next_episode(show, latest); ref.valid(); ref = next_episode(show, ref))
        if (!progress.watched(episode_at(show, ref)->id))
        {
            out.next = ref;
            out.continuing = true;
            return out;
        }
    return out;
}

} // namespace hui::media
