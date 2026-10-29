// Mediathek - Reads what a movie file contains in the background and remembers it.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/info_service.hpp"

#include "core/save_file.hpp"

#include <deque>
#include <map>
#include <pthread.h>
#include <unistd.h>
#include <utility>

namespace hui::media
{

namespace
{
// A header of a film is a few kilobytes; a cache file is far smaller than this.
constexpr std::size_t kMaxCacheFile = 16 * 1024;

struct Job
{
    std::string id;
    std::string path;
};
} // namespace

struct InfoService::Impl
{
    Impl(Probe p, std::string dir) : probe(std::move(p)), directory(std::move(dir))
    {
        save::ensure_directory(directory);
        pthread_mutex_init(&mutex, nullptr);
        pthread_cond_init(&wake, nullptr);
        pthread_create(&thread, nullptr, &Impl::run, this);
    }
    ~Impl()
    {
        pthread_mutex_lock(&mutex);
        stop = true;
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&mutex);
        pthread_join(thread, nullptr);
        pthread_cond_destroy(&wake);
        pthread_mutex_destroy(&mutex);
    }

    static void *run(void *self)
    {
        static_cast<Impl *>(self)->loop();
        return nullptr;
    }

    std::string cache_path(const std::string &id) const
    {
        return directory + "/" + id + ".info";
    }

    void loop()
    {
        for (;;)
        {
            Job job;
            pthread_mutex_lock(&mutex);
            while (!stop && queue.empty())
                pthread_cond_wait(&wake, &mutex);
            if (stop)
            {
                pthread_mutex_unlock(&mutex);
                return;
            }
            job = std::move(queue.front());
            queue.pop_front();
            pthread_mutex_unlock(&mutex);

            auto info = std::make_shared<MediaInfo>();
            std::string cached;
            bool ok = save::read_file(cache_path(job.id), &cached, kMaxCacheFile) &&
                      decode_info(cached, info.get());
            if (!ok)
            {
                *info = MediaInfo();
                ok = probe(job.path, info.get());
                if (ok)
                    save::write_atomic(cache_path(job.id), encode_info(*info));
            }

            pthread_mutex_lock(&mutex);
            Entry &entry = entries[job.id];
            entry.state = ok ? State::ready : State::failed;
            if (ok)
                entry.info = std::move(info);
            pthread_mutex_unlock(&mutex);
        }
    }

    struct Entry
    {
        State state = State::unknown;
        std::shared_ptr<const MediaInfo> info;
    };

    Probe probe;
    std::string directory;
    pthread_t thread{};
    mutable pthread_mutex_t mutex;
    pthread_cond_t wake;
    std::deque<Job> queue;
    std::map<std::string, Entry> entries;
    bool stop = false;
};

InfoService::InfoService(Probe probe, std::string directory)
    : impl_(std::make_unique<Impl>(std::move(probe), std::move(directory)))
{
}

InfoService::~InfoService() = default;

void InfoService::request(const std::string &id, const std::string &path)
{
    pthread_mutex_lock(&impl_->mutex);
    Impl::Entry &entry = impl_->entries[id];
    if (entry.state == State::unknown || entry.state == State::failed)
    {
        entry.state = State::pending;
        impl_->queue.push_front({id, path});
        pthread_cond_signal(&impl_->wake);
    }
    pthread_mutex_unlock(&impl_->mutex);
}

void InfoService::refresh(const std::string &id, const std::string &path)
{
    unlink(impl_->cache_path(id).c_str());
    pthread_mutex_lock(&impl_->mutex);
    Impl::Entry &entry = impl_->entries[id];
    if (entry.state == State::ready || entry.state == State::failed)
        entry.state = State::unknown; // the old info stays in `entry.info` until replaced
    pthread_mutex_unlock(&impl_->mutex);
    request(id, path);
}

InfoService::Lookup InfoService::peek(const std::string &id) const
{
    Lookup out;
    pthread_mutex_lock(&impl_->mutex);
    const auto it = impl_->entries.find(id);
    if (it != impl_->entries.end())
    {
        out.state = it->second.state;
        out.info = it->second.info;
    }
    pthread_mutex_unlock(&impl_->mutex);
    return out;
}

} // namespace hui::media
