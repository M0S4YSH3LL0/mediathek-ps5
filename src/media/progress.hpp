// Mediathek - Where each movie was left: resume points and the watched mark.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hui::media
{

struct Watch
{
    std::string id;
    double position = 0.0; // seconds
    double duration = 0.0; // seconds, 0 when unknown
    std::int64_t last = 0; // when it was last played, seconds since the epoch
    bool watched = false;
};

class ProgressStore
{
  public:
    // Past this fraction of the film a movie counts as watched and restarts
    // from the beginning, as the end credits are not worth resuming.
    static constexpr double kWatchedFraction = 0.92;
    // A few seconds in is not a resume point.
    static constexpr double kMinResume = 20.0;

    const Watch *find(std::string_view id) const;
    // Records the playback position. Marks the movie watched near the end.
    void update(std::string_view id, double position, double duration, std::int64_t now);
    void set_watched(std::string_view id, bool watched, std::int64_t now);
    void forget(std::string_view id);

    // 0..1 of the film seen, 0 for a movie never started or already watched.
    float fraction(std::string_view id) const;
    // Seconds to start from: the resume point, or 0.
    double resume_position(std::string_view id) const;
    bool in_progress(std::string_view id) const;
    bool watched(std::string_view id) const;

    // Movies in progress, newest first.
    std::vector<const Watch *> in_progress_recent() const;

    // The payload stored in a save container (save::Kind::library).
    std::string encode() const;
    // Replaces the content; false (and empty) when the payload is damaged.
    bool decode(std::string_view payload);

    // True when something changed since the last mark_saved().
    bool dirty() const
    {
        return dirty_;
    }
    void mark_saved()
    {
        dirty_ = false;
    }
    std::size_t size() const
    {
        return entries_.size();
    }

  private:
    Watch &entry(std::string_view id);

    std::vector<Watch> entries_;
    bool dirty_ = false;
};

} // namespace hui::media
