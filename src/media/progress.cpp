// Mediathek - Where each movie was left: resume points and the watched mark.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/progress.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace hui::media
{

namespace
{

// A season of watching, not a lifetime: the oldest entries go first.
constexpr std::size_t kMaxEntries = 2000;

} // namespace

const Watch *ProgressStore::find(std::string_view id) const
{
    for (const Watch &watch : entries_)
        if (watch.id == id)
            return &watch;
    return nullptr;
}

Watch &ProgressStore::entry(std::string_view id)
{
    for (Watch &watch : entries_)
        if (watch.id == id)
            return watch;
    entries_.emplace_back();
    entries_.back().id = std::string(id);
    if (entries_.size() > kMaxEntries)
    {
        const auto oldest =
            std::min_element(entries_.begin(), entries_.end() - 1,
                             [](const Watch &a, const Watch &b) { return a.last < b.last; });
        entries_.erase(oldest);
    }
    return *std::find_if(entries_.begin(), entries_.end(),
                         [&](const Watch &watch) { return watch.id == id; });
}

void ProgressStore::update(std::string_view id, double position, double duration, std::int64_t now)
{
    if (id.empty() || position < 0.0)
        return;
    Watch &watch = entry(id);
    watch.position = position;
    if (duration > 0.0)
        watch.duration = duration;
    watch.last = now;
    if (watch.duration > 0.0 && position >= watch.duration * kWatchedFraction)
    {
        watch.watched = true;
        watch.position = 0.0;
    }
    else if (position >= kMinResume)
    {
        watch.watched = false;
    }
    dirty_ = true;
}

void ProgressStore::set_watched(std::string_view id, bool watched, std::int64_t now)
{
    Watch &watch = entry(id);
    watch.watched = watched;
    watch.position = 0.0;
    watch.last = now;
    dirty_ = true;
}

void ProgressStore::forget(std::string_view id)
{
    const auto it = std::remove_if(entries_.begin(), entries_.end(),
                                   [&](const Watch &watch) { return watch.id == id; });
    if (it != entries_.end())
    {
        entries_.erase(it, entries_.end());
        dirty_ = true;
    }
}

float ProgressStore::fraction(std::string_view id) const
{
    const Watch *watch = find(id);
    if (watch == nullptr || watch->watched || watch->duration <= 0.0)
        return 0.0f;
    return static_cast<float>(std::clamp(watch->position / watch->duration, 0.0, 1.0));
}

double ProgressStore::resume_position(std::string_view id) const
{
    const Watch *watch = find(id);
    if (watch == nullptr || watch->watched || watch->position < kMinResume)
        return 0.0;
    return watch->position;
}

bool ProgressStore::in_progress(std::string_view id) const
{
    return resume_position(id) > 0.0;
}

bool ProgressStore::watched(std::string_view id) const
{
    const Watch *watch = find(id);
    return watch != nullptr && watch->watched;
}

std::vector<const Watch *> ProgressStore::in_progress_recent() const
{
    std::vector<const Watch *> list;
    for (const Watch &watch : entries_)
        if (!watch.watched && watch.position >= kMinResume)
            list.push_back(&watch);
    std::sort(list.begin(), list.end(),
              [](const Watch *a, const Watch *b) { return a->last > b->last; });
    return list;
}

std::string ProgressStore::encode() const
{
    std::string out;
    char line[96];
    for (const Watch &watch : entries_)
    {
        std::snprintf(line, sizeof(line), "%s %.1f %.1f %lld %d\n", watch.id.c_str(),
                      watch.position, watch.duration, static_cast<long long>(watch.last),
                      watch.watched ? 1 : 0);
        out += line;
    }
    return out;
}

bool ProgressStore::decode(std::string_view payload)
{
    entries_.clear();
    std::size_t start = 0;
    while (start < payload.size())
    {
        std::size_t end = payload.find('\n', start);
        if (end == std::string_view::npos)
            end = payload.size();
        const std::string line(payload.substr(start, end - start));
        start = end + 1;
        if (line.empty())
            continue;
        char id[32] = {};
        double position = 0.0;
        double duration = 0.0;
        long long last = 0;
        int watched = 0;
        if (std::sscanf(line.c_str(), "%31s %lf %lf %lld %d", id, &position, &duration, &last,
                        &watched) != 5)
        {
            entries_.clear();
            return false;
        }
        Watch watch;
        watch.id = id;
        watch.position = position;
        watch.duration = duration;
        watch.last = last;
        watch.watched = watched != 0;
        entries_.push_back(std::move(watch));
    }
    dirty_ = false;
    return true;
}

} // namespace hui::media
