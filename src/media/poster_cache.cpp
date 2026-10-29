// Mediathek - Artwork loading: decoded off the frame thread, uploaded a few per frame.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/poster_cache.hpp"

#include "core/save_file.hpp"
#include "third_party/stb/image.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <pthread.h>

namespace hui::media
{

namespace
{

// Artwork files are small; a 40 MB "poster" is a mistake, not a poster.
constexpr std::size_t kMaxFileBytes = 24u << 20;
constexpr int kUploadsPerFrame = 2;

// The average colour, lifted toward a usable accent: dark pictures give a
// muddy tint and grey ones none, so brightness and saturation get a floor.
gfx::Color tone_of(const std::uint8_t *rgba, int width, int height)
{
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double weight = 0.0;
    const int step = std::max(1, std::min(width, height) / 24);
    for (int y = 0; y < height; y += step)
        for (int x = 0; x < width; x += step)
        {
            const std::uint8_t *p = rgba + (static_cast<std::size_t>(y) * width + x) * 4;
            const double max = std::max({p[0], p[1], p[2]});
            const double min = std::min({p[0], p[1], p[2]});
            // Saturated pixels say more about a picture's colour than greys.
            const double w = 0.2 + (max - min) / 255.0;
            r += p[0] * w;
            g += p[1] * w;
            b += p[2] * w;
            weight += w;
        }
    if (weight <= 0.0)
        return gfx::Color::rgb(0x4a5a8a);
    gfx::Color c{static_cast<float>(r / weight / 255.0), static_cast<float>(g / weight / 255.0),
                 static_cast<float>(b / weight / 255.0), 1.0f};
    const float max = std::max({c.r, c.g, c.b});
    const float min = std::min({c.r, c.g, c.b});
    const float grey = (c.r + c.g + c.b) / 3.0f;
    const float boost = max - min < 0.18f ? 1.0f : 0.0f; // nearly grey: tint it blue
    c.r = grey + (c.r - grey) * 1.35f;
    c.g = grey + (c.g - grey) * 1.35f;
    c.b = grey + (c.b - grey) * 1.35f + 0.10f * boost;
    const float peak = std::max({c.r, c.g, c.b, 0.001f});
    const float scale = peak < 0.55f ? 0.55f / peak : 1.0f;
    c.r = std::clamp(c.r * scale, 0.0f, 1.0f);
    c.g = std::clamp(c.g * scale, 0.0f, 1.0f);
    c.b = std::clamp(c.b * scale, 0.0f, 1.0f);
    return c;
}

} // namespace

bool decode_image_file(const std::string &path, int max_width, int max_height, DecodedImage *out)
{
    std::string data;
    if (!save::read_file(path, &data, kMaxFileBytes) || data.empty())
        return false;
    int width = 0;
    int height = 0;
    unsigned char *pixels = hui_image_decode(reinterpret_cast<const unsigned char *>(data.data()),
                                             static_cast<int>(data.size()), &width, &height);
    if (pixels == nullptr || width <= 0 || height <= 0)
        return false;
    int target_w = width;
    int target_h = height;
    if (width > max_width || height > max_height)
    {
        const double scale = std::min(static_cast<double>(max_width) / width,
                                      static_cast<double>(max_height) / height);
        target_w = std::max(1, static_cast<int>(std::lround(width * scale)));
        target_h = std::max(1, static_cast<int>(std::lround(height * scale)));
    }
    unsigned char *scaled = pixels;
    if (target_w != width || target_h != height)
    {
        scaled = hui_image_resize(pixels, width, height, target_w, target_h);
        hui_image_free(pixels);
        if (scaled == nullptr)
            return false;
    }
    out->width = target_w;
    out->height = target_h;
    out->rgba.assign(scaled, scaled + static_cast<std::size_t>(target_w) * target_h * 4);
    out->tone = tone_of(scaled, target_w, target_h);
    hui_image_free(scaled);
    return true;
}

// ---------------------------------------------------------------------------

struct PosterCache::Impl
{
    enum class State : std::uint8_t
    {
        queued,
        decoding,
        decoded, // waiting for the frame thread to upload it
        ready,
        failed,
    };
    struct Slot
    {
        std::string path;
        ArtKind kind = ArtKind::poster;
        std::atomic<State> state{State::queued}; // the worker writes it, the frame thread reads it
        DecodedImage image;
        Art art;
        std::uint64_t used = 0; // frame of the last get()
    };

    Impl(Upload up, Release rel, std::size_t posters, std::size_t backdrops)
        : upload(std::move(up)), release(std::move(rel)), poster_budget(posters),
          backdrop_budget(backdrops)
    {
        pthread_mutex_init(&mutex, nullptr);
        pthread_cond_init(&wake, nullptr);
        pthread_create(&thread, nullptr, &Impl::run, this);
    }

    ~Impl()
    {
        pthread_mutex_lock(&mutex);
        stopping = true;
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&mutex);
        pthread_join(thread, nullptr);
        for (auto &entry : slots)
            if (entry.second->art.texture != 0)
                release(entry.second->art.texture);
        pthread_cond_destroy(&wake);
        pthread_mutex_destroy(&mutex);
    }

    static void *run(void *self)
    {
        static_cast<Impl *>(self)->work();
        return nullptr;
    }

    void work()
    {
        for (;;)
        {
            Slot *slot = nullptr;
            pthread_mutex_lock(&mutex);
            while (!stopping && queue.empty())
                pthread_cond_wait(&wake, &mutex);
            if (stopping)
            {
                pthread_mutex_unlock(&mutex);
                return;
            }
            // The newest request first: the picture the player is looking at
            // matters more than the ones scrolled past a second ago.
            slot = queue.back();
            queue.pop_back();
            slot->state = State::decoding;
            const std::string path = slot->path;
            const ArtKind kind = slot->kind;
            pthread_mutex_unlock(&mutex);

            DecodedImage image;
            const bool ok = kind == ArtKind::poster
                                ? decode_image_file(path, kPosterWidth, kPosterHeight, &image)
                                : decode_image_file(path, kBackdropWidth, kBackdropHeight, &image);

            pthread_mutex_lock(&mutex);
            if (ok)
            {
                slot->image = std::move(image);
                slot->state = State::decoded;
                finished.push_back(slot);
            }
            else
            {
                slot->state = State::failed;
            }
            pthread_mutex_unlock(&mutex);
        }
    }

    Upload upload;
    Release release;
    std::size_t poster_budget;
    std::size_t backdrop_budget;

    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t thread{};
    bool stopping = false;

    // Slots are heap-allocated: the worker holds raw pointers across the lock.
    std::map<std::string, std::unique_ptr<Slot>> slots;
    std::vector<Slot *> queue;    // waiting for the worker (guarded by mutex)
    std::vector<Slot *> finished; // decoded, waiting for upload (guarded by mutex)
    std::uint64_t frame = 0;
};

PosterCache::PosterCache(Upload upload, Release release, std::size_t poster_budget,
                         std::size_t backdrop_budget)
    : impl_(std::make_unique<Impl>(std::move(upload), std::move(release), poster_budget,
                                   backdrop_budget))
{
}

PosterCache::~PosterCache() = default;

const Art *PosterCache::get(const std::string &path, ArtKind kind)
{
    if (path.empty())
        return nullptr;
    Impl &impl = *impl_;
    const std::string key = (kind == ArtKind::poster ? "p:" : "b:") + path;
    auto it = impl.slots.find(key);
    if (it == impl.slots.end())
    {
        auto slot = std::make_unique<Impl::Slot>();
        slot->path = path;
        slot->kind = kind;
        Impl::Slot *raw = slot.get();
        impl.slots.emplace(key, std::move(slot));
        pthread_mutex_lock(&impl.mutex);
        impl.queue.push_back(raw);
        pthread_cond_signal(&impl.wake);
        pthread_mutex_unlock(&impl.mutex);
        it = impl.slots.find(key);
    }
    Impl::Slot &slot = *it->second;
    slot.used = impl.frame;
    if (slot.state == Impl::State::ready)
        return &slot.art;
    // A picture that fell out of the cache and is wanted again goes back in.
    return nullptr;
}

bool PosterCache::failed(const std::string &path) const
{
    for (const char *prefix : {"p:", "b:"})
    {
        const auto it = impl_->slots.find(prefix + path);
        if (it != impl_->slots.end() && it->second->state == Impl::State::failed)
            return true;
    }
    return false;
}

void PosterCache::poll()
{
    Impl &impl = *impl_;
    ++impl.frame;

    std::vector<Impl::Slot *> ready;
    pthread_mutex_lock(&impl.mutex);
    const std::size_t take = std::min<std::size_t>(impl.finished.size(), kUploadsPerFrame);
    ready.assign(impl.finished.begin(), impl.finished.begin() + static_cast<std::ptrdiff_t>(take));
    impl.finished.erase(impl.finished.begin(),
                        impl.finished.begin() + static_cast<std::ptrdiff_t>(take));
    pthread_mutex_unlock(&impl.mutex);

    for (Impl::Slot *slot : ready)
    {
        slot->art.texture =
            impl.upload(slot->image.width, slot->image.height, slot->image.rgba.data());
        slot->art.width = slot->image.width;
        slot->art.height = slot->image.height;
        slot->art.tone = slot->image.tone;
        slot->image = DecodedImage{};
        slot->state = slot->art.texture != 0 ? Impl::State::ready : Impl::State::failed;
    }

    // Over budget: the least recently wanted textures go. Entries that are
    // still wanted this frame (used == frame - 1 or newer) are never evicted.
    for (const ArtKind kind : {ArtKind::poster, ArtKind::backdrop})
    {
        const std::size_t budget =
            kind == ArtKind::poster ? impl.poster_budget : impl.backdrop_budget;
        std::vector<Impl::Slot *> resident;
        for (auto &entry : impl.slots)
            if (entry.second->kind == kind && entry.second->state == Impl::State::ready)
                resident.push_back(entry.second.get());
        if (resident.size() <= budget)
            continue;
        std::sort(resident.begin(), resident.end(),
                  [](const Impl::Slot *a, const Impl::Slot *b) { return a->used < b->used; });
        for (std::size_t i = 0; i + budget < resident.size(); ++i)
        {
            Impl::Slot *slot = resident[i];
            if (slot->used + 1 >= impl.frame)
                break;
            impl.release(slot->art.texture);
            // Forgetting the slot lets get() queue it again if it is wanted later.
            for (auto it = impl.slots.begin(); it != impl.slots.end(); ++it)
                if (it->second.get() == slot)
                {
                    impl.slots.erase(it);
                    break;
                }
        }
    }
}

void PosterCache::clear()
{
    Impl &impl = *impl_;
    pthread_mutex_lock(&impl.mutex);
    impl.queue.clear();
    impl.finished.clear();
    pthread_mutex_unlock(&impl.mutex);
    // A slot the worker is decoding right now cannot be freed under it: wait
    // for it by keeping decoding slots, and drop the rest.
    for (auto it = impl.slots.begin(); it != impl.slots.end();)
    {
        Impl::Slot &slot = *it->second;
        if (slot.state == Impl::State::decoding)
        {
            ++it;
            continue;
        }
        if (slot.art.texture != 0)
            impl.release(slot.art.texture);
        it = impl.slots.erase(it);
    }
}

std::size_t PosterCache::loaded() const
{
    std::size_t count = 0;
    for (const auto &entry : impl_->slots)
        if (entry.second->state == Impl::State::ready)
            ++count;
    return count;
}

std::size_t PosterCache::pending() const
{
    std::size_t count = 0;
    for (const auto &entry : impl_->slots)
        if (entry.second->state == Impl::State::queued ||
            entry.second->state == Impl::State::decoding ||
            entry.second->state == Impl::State::decoded)
            ++count;
    return count;
}

} // namespace hui::media
