// Mediathek - Reads what a movie file contains in the background and remembers it.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/mediainfo.hpp"

#include <functional>
#include <memory>
#include <string>

namespace hui::media
{

// A worker thread that probes one file at a time (a drive that has to spin up
// must not hold the frame thread) and keeps each answer in `directory`, one
// file per movie id, so a file is read once and not on every start. The frame
// thread asks (request) and looks (peek); it never waits.
class InfoService
{
  public:
    // Fills *out from the file's headers; false when it cannot be read.
    using Probe = std::function<bool(const std::string &path, MediaInfo *out)>;

    enum class State : unsigned char
    {
        unknown, // never asked for
        pending, // queued or being read
        ready,
        failed, // the file could not be read; asking again tries again
    };
    struct Lookup
    {
        State state = State::unknown;
        std::shared_ptr<const MediaInfo> info; // set when ready
    };

    InfoService(Probe probe, std::string directory);
    ~InfoService();
    InfoService(const InfoService &) = delete;
    InfoService &operator=(const InfoService &) = delete;

    // Starts reading this movie's file unless it is known or under way. The newest
    // request is served first: the page the viewer looks at is the one that matters.
    void request(const std::string &id, const std::string &path);
    // Reads the file again (it was replaced, or the viewer asked); the old answer stays
    // visible until the new one is in.
    void refresh(const std::string &id, const std::string &path);
    Lookup peek(const std::string &id) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hui::media
