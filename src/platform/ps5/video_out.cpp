// Mediathek - The console's video output beside ps5-opengl: what the TV gets, and HDR10 on and off.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5-opengl opens the output and registers the screen's buffers itself (B8G8R8A8, 64 KiB
// tiles) and keeps the handle to itself. The build routes its sceVideoOutOpen through the
// wrapper here (--wrap in tools/build.sh), which keeps the handle. HDR10 is then what VLC's
// PS5 port does (GPL-2.0-or-later, https://github.com/ZiZc3/VLC, patches/mesa): the buffers
// stay registered and are given the HDR10 pixel format in place, and the app writes
// 10:10:10:2 words into the same 32 bits a pixel has.

#include "platform/ps5/video_out.hpp"

#include "platform/ps5/system.hpp"

#include <atomic>
#include <cstdint>

namespace
{

// A pixel format, as sceVideoOutSetBufferAttribute2 takes it: what ps5-opengl registers, and
// HDR10 in the same channel order.
constexpr std::uint64_t kFormatSdr = UINT64_C(0x8000000000000000);
constexpr std::uint64_t kFormatHdr10 = UINT64_C(0x8100070400000000);
constexpr std::uint32_t kTiling64KiB = 0;
// How long a flip may still be on its way before the switch goes ahead anyway.
constexpr int kFlipWaitMs = 100;

struct Attribute
{
    std::uint8_t bytes[80];
};

// sceVideoOutGetOutputStatus's record (48 bytes, as ps5-opengl and VLC's port declare it).
struct OutputStatus
{
    std::uint32_t resolution = 0;
    std::uint32_t dynamic_range = 0; // 0 unknown, 1 SDR, 2 HDR
    std::uint64_t refresh_rate = 0;
    std::uint64_t flags = 0;
    std::uint32_t mode = 0;
    std::uint32_t reserved[5] = {};
};
static_assert(sizeof(OutputStatus) == 48, "VideoOut status record");

std::atomic<int> g_handle{-1};

} // namespace

extern "C"
{
    int __real_sceVideoOutOpen(std::int32_t user, std::int32_t bus, std::int32_t index,
                               const void *parameter);
    int sceVideoOutGetOutputStatus(std::int32_t handle, OutputStatus *status);
    void sceVideoOutSetBufferAttribute2(Attribute *attribute, std::uint64_t pixel_format,
                                        std::uint32_t tiling, std::uint32_t width,
                                        std::uint32_t height, std::uint64_t option,
                                        std::uint32_t dcc_control, std::uint64_t dcc_clear);
    int sceVideoOutSubmitChangeBufferAttribute2(std::int32_t handle, std::int32_t set,
                                                const Attribute *attribute, const void *option);
    int sceVideoOutIsFlipPending(std::int32_t handle);

    int __wrap_sceVideoOutOpen(std::int32_t user, std::int32_t bus, std::int32_t index,
                               const void *parameter)
    {
        const int handle = __real_sceVideoOutOpen(user, bus, index, parameter);
        if (handle >= 0)
            g_handle.store(handle);
        hui::sys::log("[HUI] video out handle=%d", handle);
        return handle;
    }
}

namespace hui::ps5::video_out
{

int dynamic_range()
{
    const int handle = g_handle.load();
    if (handle < 0)
        return -1;
    OutputStatus status;
    const int rc = sceVideoOutGetOutputStatus(handle, &status);
    return rc == 0 ? static_cast<int>(status.dynamic_range) : -1;
}

bool set_hdr(bool on, int width, int height)
{
    const int handle = g_handle.load();
    if (handle < 0 || width <= 0 || height <= 0)
        return false;
    int waited = 0;
    while (sceVideoOutIsFlipPending(handle) > 0 && waited < kFlipWaitMs)
    {
        sys::sleep_us(1000);
        ++waited;
    }
    Attribute attribute{};
    sceVideoOutSetBufferAttribute2(&attribute, on ? kFormatHdr10 : kFormatSdr, kTiling64KiB,
                                   static_cast<std::uint32_t>(width),
                                   static_cast<std::uint32_t>(height), 0, 0, 0);
    const int rc = sceVideoOutSubmitChangeBufferAttribute2(handle, 0, &attribute, nullptr);
    sys::log("[HUI] hdr output %s: 0x%08x (waited %d ms for a flip)", on ? "on" : "off",
             static_cast<unsigned>(rc), waited);
    return rc == 0;
}

} // namespace hui::ps5::video_out
