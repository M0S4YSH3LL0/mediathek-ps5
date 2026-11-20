// Mediathek - The console's direct memory as blocks for decoded pictures.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/direct_memory.hpp"

#include "platform/ps5/system.hpp"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>

extern "C"
{
    std::int64_t sceKernelGetDirectMemorySize(void);
    int sceKernelAllocateDirectMemory(std::int64_t from, std::int64_t to, std::size_t size,
                                      std::size_t alignment, int type, std::int64_t *start);
    int sceKernelMapDirectMemory(void **address, std::size_t size, int protection, int flags,
                                 std::int64_t start, std::size_t alignment);
    int sceKernelReleaseDirectMemory(std::int64_t start, std::size_t size);
    int sceKernelMunmap(void *address, std::size_t size);

    std::size_t hui_heap_capacity(void);
    void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes, std::size_t *blocks,
                        std::size_t *failures);
}

namespace hui::ps5
{

namespace
{

// The type and the mapping the hardware decoder's pictures use (hw_decoder.cpp), which the
// CPU reads at 4K60 without trouble: the memory is cached, as dav1d's reference reads need.
constexpr int kDirectType = 12;
constexpr int kCpuAndGraphics = 0x33;
constexpr std::size_t kAlignment = 0x4000;
// What the heap keeps free when it lends a block: the player's queues and dav1d's own tables.
constexpr std::size_t kHeapReserve = std::size_t{96} << 20;

struct Mapping
{
    std::int64_t start = -1; // -1: from the heap
    std::size_t size = 0;
};

// Called by the pool under its lock: no lock of its own.
struct Blocks
{
    std::map<void *, Mapping> mappings;
    bool told_full = false;

    void *allocate(std::size_t bytes)
    {
        const std::size_t size = (bytes + kAlignment - 1) & ~(kAlignment - 1);
        std::int64_t start = -1;
        int code = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size,
                                                 kAlignment, kDirectType, &start);
        if (code == 0)
        {
            void *address = nullptr;
            code = sceKernelMapDirectMemory(&address, size, kCpuAndGraphics, 0, start, kAlignment);
            if (code == 0)
            {
                mappings[address] = {start, size};
                return address;
            }
            sceKernelReleaseDirectMemory(start, size);
        }
        return from_heap(bytes, code);
    }

    void *from_heap(std::size_t bytes, int code)
    {
        std::size_t live = 0;
        hui_heap_stats(&live, nullptr, nullptr, nullptr);
        const std::size_t capacity = hui_heap_capacity();
        const bool room = capacity == 0 || capacity > live + bytes + kHeapReserve;
        if (!told_full)
            sys::log("[HUI] av1: no direct memory for a %zu KiB picture (0x%08x), %s", bytes >> 10,
                     static_cast<unsigned>(code),
                     room ? "using the heap" : "and no room in the heap");
        told_full = true;
        void *address = nullptr;
        if (!room || posix_memalign(&address, 64, bytes) != 0)
            return nullptr;
        mappings[address] = {-1, bytes};
        return address;
    }

    void release(void *address)
    {
        const auto found = mappings.find(address);
        if (found == mappings.end())
            return;
        if (found->second.start < 0)
        {
            std::free(address);
        }
        else
        {
            sceKernelMunmap(address, found->second.size);
            sceKernelReleaseDirectMemory(found->second.start, found->second.size);
        }
        mappings.erase(found);
    }
};

} // namespace

media::PicturePool::Backend direct_picture_memory()
{
    auto blocks = std::make_shared<Blocks>();
    media::PicturePool::Backend backend;
    backend.allocate = [blocks](std::size_t bytes) { return blocks->allocate(bytes); };
    backend.release = [blocks](void *address, std::size_t) { blocks->release(address); };
    return backend;
}

} // namespace hui::ps5
