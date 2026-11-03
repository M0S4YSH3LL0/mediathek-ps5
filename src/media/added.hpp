// Mediathek - When the library first saw each file: what "Recently Added" orders by.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/library.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace hui::media
{

// A file's modification time says when it was made, not when it came to the console: a copy
// usually keeps the original's (an FTP upload, a copy with its times kept), so a film copied
// today could be among the oldest. This store remembers when a scan first found each film and
// episode, by id (name and size, so a move to another drive keeps it), and that is what
// `Movie::added` and `Episode::added` become.
class AddedStore
{
  public:
    // Gives every film and episode of the library the time it was first seen, and a series its
    // newest episode's. A file never seen before is recorded at `now`; before there is any
    // record (the first start with this store) every file is recorded at its own time, capped
    // at `now`, as the best guess there is. True when something was recorded: save then.
    bool apply(Library *library, std::int64_t now);

    // "mediathek-added 1", then one "id seconds" a line.
    std::string encode() const;
    // False (and nothing recorded) for text that is not such a file.
    bool decode(std::string_view text);

    std::size_t size() const
    {
        return seen_.size();
    }

  private:
    std::map<std::string, std::int64_t, std::less<>> seen_;
    bool started_ = false; // a record exists: new files are new
};

} // namespace hui::media
