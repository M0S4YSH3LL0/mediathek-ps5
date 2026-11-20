// Mediathek - Blocks of memory for decoded pictures, kept and handed out again.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <pthread.h>

#include <cstddef>
#include <functional>
#include <vector>

namespace hui::media
{

// A software AV1 decoder keeps up to eight reference pictures and a few in flight, about
// 25 MB each for 4K 10-bit: more than the app's fixed heap has to spare. The console's
// direct memory, which the hardware decoder's pictures use too, does have it, but each
// block is a system call to get and give back. So blocks are kept once had and handed out
// again, the smallest that fits first. Any thread may take and give.
class PicturePool
{
  public:
    struct Backend
    {
        std::function<void *(std::size_t bytes)> allocate; // null when there is none
        std::function<void(void *address, std::size_t bytes)> release;
    };

    // At most `max_blocks` blocks at a time. A film changing its picture size mid-way
    // asks for bigger blocks: the smaller ones not in use then make room.
    PicturePool(Backend backend, std::size_t max_blocks);
    // Every block goes back to the backend; all must have been given back by then.
    ~PicturePool();
    PicturePool(const PicturePool &) = delete;
    PicturePool &operator=(const PicturePool &) = delete;

    // A block of at least `bytes`; null when none is free and no new one can be had.
    void *take(std::size_t bytes);
    // `address` is one take() returned; it may be handed out again.
    void give(void *address);

    std::size_t blocks() const;
    std::size_t bytes() const; // in all blocks, used or not
    std::size_t in_use() const;

  private:
    struct Block
    {
        void *address = nullptr;
        std::size_t size = 0;
        bool used = false;
    };

    Backend backend_;
    std::size_t max_blocks_;
    std::vector<Block> blocks_;
    mutable pthread_mutex_t mutex_;
};

} // namespace hui::media
