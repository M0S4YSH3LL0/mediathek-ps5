// Mediathek - SubRip (.srt) subtitles: parsing and lookup by time.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hui::media
{

struct Cue
{
    double start = 0.0; // seconds
    double end = 0.0;
    std::string text; // lines joined with '\n', markup removed
};

class Subtitles
{
  public:
    // Reads SubRip text (UTF-8, with or without a byte-order mark). Cues that
    // cannot be read are skipped; false when none could be.
    bool parse(std::string_view text);

    // The cue showing at this time, or null.
    const Cue *at(double seconds) const;
    bool empty() const
    {
        return cues_.empty();
    }
    std::size_t size() const
    {
        return cues_.size();
    }

  private:
    std::vector<Cue> cues_; // ordered by start
    mutable std::size_t last_ = 0;
};

} // namespace hui::media
