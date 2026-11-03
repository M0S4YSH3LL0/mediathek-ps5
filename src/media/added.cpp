// Mediathek - When the library first saw each file: what "Recently Added" orders by.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/added.hpp"

#include <algorithm>
#include <cstdlib>

namespace hui::media
{

namespace
{

constexpr std::string_view kHeader = "mediathek-added 1";

} // namespace

bool AddedStore::apply(Library *library, std::int64_t now)
{
    bool changed = !started_;
    const auto first_seen = [&](const std::string &id, std::int64_t file_time) -> std::int64_t
    {
        if (const auto it = seen_.find(id); it != seen_.end())
            return it->second;
        // Without a record, every file would be "new" at once and in no order: its own time
        // is the better guess. A time in the future (a wrong clock where it was made) is not.
        const std::int64_t when = started_ ? now : std::min(file_time, now);
        seen_.emplace(id, when);
        changed = true;
        return when;
    };
    library->edit_all(
        [&](Movie *movie)
        {
            if (!movie->show)
            {
                movie->added = first_seen(movie->id, movie->added);
                return;
            }
            movie->added = 0;
            for (Season &season : movie->seasons)
                for (Episode &episode : season.episodes)
                {
                    episode.added = first_seen(episode.id, episode.added);
                    movie->added = std::max(movie->added, episode.added);
                }
        });
    started_ = true;
    return changed;
}

std::string AddedStore::encode() const
{
    std::string out(kHeader);
    out += '\n';
    for (const auto &[id, when] : seen_)
    {
        out += id;
        out += ' ';
        out += std::to_string(when);
        out += '\n';
    }
    return out;
}

bool AddedStore::decode(std::string_view text)
{
    if (text.substr(0, kHeader.size()) != kHeader)
        return false;
    std::map<std::string, std::int64_t, std::less<>> seen;
    std::size_t start = text.find('\n');
    while (start != std::string_view::npos && start < text.size())
    {
        ++start;
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string_view line = text.substr(start, end - start);
        start = end;
        const std::size_t space = line.find(' ');
        if (line.empty() || space == 0 || space == std::string_view::npos || space > 64)
            continue; // a damaged line costs that file its date, not the others theirs
        const std::string number(line.substr(space + 1));
        char *rest = nullptr;
        const long long when = std::strtoll(number.c_str(), &rest, 10);
        if (rest == number.c_str() || *rest != '\0')
            continue;
        seen.emplace(std::string(line.substr(0, space)), static_cast<std::int64_t>(when));
    }
    seen_ = std::move(seen);
    started_ = true;
    return true;
}

} // namespace hui::media
