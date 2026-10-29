// Mediathek - Finding the movies: the helper payload that lists folders for the sandboxed app.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/library.hpp"

#include <cstdint>
#include <string>

namespace hui::ps5
{

// The app cannot list folders from inside its sandbox, so a small payload
// (helper/mt-scan.c) does it: the app sends it to the ELF loader (etaHEN's
// port 9021), and it writes an index file into the title's install folder,
// which the app reads as /app0/mediathek-index.txt.
class Scanner
{
  public:
    enum class Status : std::uint8_t
    {
        idle,
        running,
        done,
        failed,
    };

    // The index the last scan left behind, so the library is there at once.
    static bool load_cached(media::ScanResult *out);

    // Sends the payload. False (with a sentence in note()) when it cannot:
    // the payload file is missing, or nothing answers on the loader's port.
    bool start();
    // Once per frame while running: looks for the new index every half second.
    Status poll(float dt, media::ScanResult *out);

    const std::string &note() const
    {
        return note_;
    }

  private:
    Status status_ = Status::idle;
    std::int64_t started_ = 0;
    float since_poll_ = 0.0f;
    float waited_ = 0.0f;
    std::string note_;
};

} // namespace hui::ps5
