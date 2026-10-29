// Mediathek - Fetches covers and wallpapers in the background, a film at a time.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/tmdb.hpp"

#include <memory>
#include <string>
#include <vector>

namespace hui::media
{

// A worker thread that works through a queue of CoverJobs. The frame thread
// only enqueues and collects; it never waits for the network.
class CoverService
{
  public:
    struct Done
    {
        std::string id;
        bool replaced = false; // asked for again: a texture of the old picture may be cached
    };

    CoverService(TmdbConfig config, HttpGet get, std::string directory);
    ~CoverService();
    CoverService(const CoverService &) = delete;
    CoverService &operator=(const CoverService &) = delete;

    const CoverStore &store() const;

    // The jobs to do now, replacing what is still queued (a rescan found them).
    void enqueue(std::vector<CoverJob> jobs);
    // Fetches this film's pictures again, ahead of everything else.
    void redo(const CoverJob &job);

    // Films whose pictures arrived since the last call.
    std::vector<Done> take_done();
    // One sentence when something stops the work ("No TMDB_API_KEY..."), else "".
    std::string problem() const;
    // Jobs not yet finished, the running one included.
    std::size_t pending() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hui::media
