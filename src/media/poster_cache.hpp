// Mediathek - Artwork loading: decoded off the frame thread, uploaded a few per frame.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gfx/draw_list.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hui::media
{

// A decoded picture, already scaled down to what the screen needs.
struct DecodedImage
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    gfx::Color tone; // the picture's average colour, lifted: tints the page behind it
};

// Reads a JPEG or PNG and scales it to fit max_width x max_height (never up).
// Returns false when the file is missing or not a picture.
bool decode_image_file(const std::string &path, int max_width, int max_height, DecodedImage *out);

enum class ArtKind : std::uint8_t
{
    poster,   // portrait cover, kept small: there are many on screen
    backdrop, // wide picture behind the hero panel
};

struct Art
{
    std::uint32_t texture = 0;
    int width = 0;
    int height = 0;
    gfx::Color tone;
};

// Loads pictures on a worker thread and keeps the most recently used ones as
// textures. get() never blocks: it answers null until the picture has been
// decoded and uploaded, and asks again each frame it is wanted.
class PosterCache
{
  public:
    using Upload = std::function<std::uint32_t(int, int, const std::uint8_t *)>;
    using Release = std::function<void(std::uint32_t)>;

    static constexpr int kPosterWidth = 300;
    static constexpr int kPosterHeight = 450;
    static constexpr int kBackdropWidth = 1280;
    static constexpr int kBackdropHeight = 720;

    PosterCache(Upload upload, Release release, std::size_t poster_budget = 160,
                std::size_t backdrop_budget = 3);
    PosterCache(const PosterCache &) = delete;
    PosterCache &operator=(const PosterCache &) = delete;
    ~PosterCache();

    // Null while loading, and for good when the file cannot be read.
    const Art *get(const std::string &path, ArtKind kind);
    bool failed(const std::string &path) const;

    // Once per frame on the thread that owns the GL context: uploads what the
    // worker finished (a few per frame, so scrolling never hitches) and drops
    // the least recently used textures over budget.
    void poll();

    // Drops everything (the library changed) and forgets queued work.
    void clear();

    std::size_t loaded() const;
    std::size_t pending() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hui::media
