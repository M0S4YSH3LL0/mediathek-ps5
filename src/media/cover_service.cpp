// Mediathek - Fetches covers and wallpapers in the background, a film at a time.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/cover_service.hpp"

#include "core/i18n.hpp"
#include "core/save_file.hpp"

#include <deque>
#include <pthread.h>
#include <unistd.h>

namespace hui::media
{

namespace
{
// Two failed requests in a row mean the network is down; a long queue must
// not spend minutes finding that out one film at a time.
constexpr int kGiveUpAfterFailures = 3;
constexpr useconds_t kPauseBetweenFilms = 120 * 1000;
} // namespace

struct CoverService::Impl
{
    Impl(TmdbConfig c, HttpGet g, std::string directory)
        : config(std::move(c)), get(std::move(g)), store(std::move(directory))
    {
        save::ensure_directory(store.directory());
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

    void loop()
    {
        int failures = 0;
        for (;;)
        {
            CoverJob job;
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
            running = true;
            pthread_mutex_unlock(&mutex);

            std::string note;
            const FetchOutcome outcome = fetch_cover(config, get, store, job, &note);

            pthread_mutex_lock(&mutex);
            running = false;
            if (outcome == FetchOutcome::stored)
            {
                done.push_back({job.id, job.again});
                failures = 0;
                trouble.clear();
            }
            else if (outcome == FetchOutcome::no_match)
            {
                failures = 0;
            }
            else if (outcome == FetchOutcome::no_key)
            {
                queue.clear();
                trouble = note;
            }
            else if (++failures >= kGiveUpAfterFailures)
            {
                queue.clear();
                failures = 0;
                trouble = i18n::format(i18n::Text::tmdb_unreachable, note);
            }
            pthread_mutex_unlock(&mutex);
            usleep(kPauseBetweenFilms);
        }
    }

    TmdbConfig config;
    HttpGet get;
    CoverStore store;
    pthread_t thread{};
    mutable pthread_mutex_t mutex;
    pthread_cond_t wake;
    std::deque<CoverJob> queue;
    std::vector<Done> done;
    std::string trouble;
    bool running = false;
    bool stop = false;
};

CoverService::CoverService(TmdbConfig config, HttpGet get, std::string directory)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(get), std::move(directory)))
{
}

CoverService::~CoverService() = default;

const CoverStore &CoverService::store() const
{
    return impl_->store;
}

void CoverService::enqueue(std::vector<CoverJob> jobs)
{
    pthread_mutex_lock(&impl_->mutex);
    impl_->queue.assign(std::make_move_iterator(jobs.begin()), std::make_move_iterator(jobs.end()));
    impl_->trouble.clear();
    pthread_cond_signal(&impl_->wake);
    pthread_mutex_unlock(&impl_->mutex);
}

void CoverService::redo(const CoverJob &job)
{
    impl_->store.forget(job.id);
    pthread_mutex_lock(&impl_->mutex);
    impl_->queue.push_front(job);
    impl_->trouble.clear();
    pthread_cond_signal(&impl_->wake);
    pthread_mutex_unlock(&impl_->mutex);
}

std::vector<CoverService::Done> CoverService::take_done()
{
    pthread_mutex_lock(&impl_->mutex);
    std::vector<Done> out;
    out.swap(impl_->done);
    pthread_mutex_unlock(&impl_->mutex);
    return out;
}

std::string CoverService::problem() const
{
    pthread_mutex_lock(&impl_->mutex);
    std::string text = impl_->trouble;
    pthread_mutex_unlock(&impl_->mutex);
    return text;
}

std::size_t CoverService::pending() const
{
    pthread_mutex_lock(&impl_->mutex);
    const std::size_t count = impl_->queue.size() + (impl_->running ? 1 : 0);
    pthread_mutex_unlock(&impl_->mutex);
    return count;
}

} // namespace hui::media
