// Mediathek - Blocks of memory for decoded pictures, kept and handed out again.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/picture_pool.hpp"

#include <algorithm>
#include <utility>

namespace hui::media
{

namespace
{

class Guard
{
  public:
    explicit Guard(pthread_mutex_t &mutex) : mutex_(mutex)
    {
        pthread_mutex_lock(&mutex_);
    }
    ~Guard()
    {
        pthread_mutex_unlock(&mutex_);
    }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

  private:
    pthread_mutex_t &mutex_;
};

} // namespace

PicturePool::PicturePool(Backend backend, std::size_t max_blocks)
    : backend_(std::move(backend)), max_blocks_(max_blocks)
{
    pthread_mutex_init(&mutex_, nullptr);
}

PicturePool::~PicturePool()
{
    for (const Block &block : blocks_)
        backend_.release(block.address, block.size);
    pthread_mutex_destroy(&mutex_);
}

void *PicturePool::take(std::size_t bytes)
{
    Guard guard(mutex_);
    Block *best = nullptr;
    for (Block &block : blocks_)
        if (!block.used && block.size >= bytes && (best == nullptr || block.size < best->size))
            best = &block;
    if (best != nullptr)
    {
        best->used = true;
        return best->address;
    }
    // Full: free blocks too small for this size are of no use any more.
    if (blocks_.size() >= max_blocks_)
    {
        auto unused = std::find_if(blocks_.begin(), blocks_.end(),
                                   [](const Block &block) { return !block.used; });
        if (unused == blocks_.end())
            return nullptr;
        backend_.release(unused->address, unused->size);
        blocks_.erase(unused);
    }
    void *address = backend_.allocate(bytes);
    if (address == nullptr)
        return nullptr;
    Block block;
    block.address = address;
    block.size = bytes;
    block.used = true;
    blocks_.push_back(block);
    return address;
}

void PicturePool::give(void *address)
{
    Guard guard(mutex_);
    for (Block &block : blocks_)
        if (block.address == address)
        {
            block.used = false;
            return;
        }
}

std::size_t PicturePool::blocks() const
{
    Guard guard(mutex_);
    return blocks_.size();
}

std::size_t PicturePool::bytes() const
{
    Guard guard(mutex_);
    std::size_t total = 0;
    for (const Block &block : blocks_)
        total += block.size;
    return total;
}

std::size_t PicturePool::in_use() const
{
    Guard guard(mutex_);
    return static_cast<std::size_t>(std::count_if(blocks_.begin(), blocks_.end(),
                                                  [](const Block &block) { return block.used; }));
}

} // namespace hui::media
