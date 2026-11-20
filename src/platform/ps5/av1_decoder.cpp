// Mediathek - AV1 pictures from dav1d, for the films PacBrew's FFmpeg cannot decode.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// No system calls here, so the PC's tests decode real AV1 with the same code.

#include "platform/ps5/av1_decoder.hpp"

#include <dav1d/dav1d.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace hui::ps5
{

namespace
{

// Rounds of send and get one unit may take. Each round either takes data or gives a
// picture, so a unit is through in two or three; this only keeps a decoder that broke its
// word from holding the player forever.
constexpr int kMaxRounds = 64;

void log_line(void *cookie, const char *format, va_list arguments)
{
    const auto *log = static_cast<const std::function<void(const char *)> *>(cookie);
    if (log == nullptr || !*log)
        return;
    char line[256];
    std::vsnprintf(line, sizeof(line), format, arguments);
    const std::size_t length = std::strlen(line);
    if (length > 0 && line[length - 1] == '\n')
        line[length - 1] = '\0';
    (*log)(line);
}

// Lays the picture out as dav1d's own allocator does (src/picture.c): rows and the height
// padded to 128 pixels, the planes 64-byte aligned, and a stride that is a multiple of 1024
// bytes nudged off it, which would map rows of a block to the same cache sets.
int take_picture(Dav1dPicture *picture, void *cookie)
{
    auto *pool = static_cast<media::PicturePool *>(cookie);
    const int high = picture->p.bpc > 8 ? 1 : 0;
    const int aligned_width = (picture->p.w + 127) & ~127;
    const int aligned_height = (picture->p.h + 127) & ~127;
    const bool chroma = picture->p.layout != DAV1D_PIXEL_LAYOUT_I400;
    const int half_rows = picture->p.layout == DAV1D_PIXEL_LAYOUT_I420 ? 1 : 0;
    const int half_columns = picture->p.layout != DAV1D_PIXEL_LAYOUT_I444 ? 1 : 0;
    ptrdiff_t luma_stride = static_cast<ptrdiff_t>(aligned_width) << high;
    ptrdiff_t chroma_stride = chroma ? luma_stride >> half_columns : 0;
    if ((luma_stride & 1023) == 0)
        luma_stride += DAV1D_PICTURE_ALIGNMENT;
    if (chroma && (chroma_stride & 1023) == 0)
        chroma_stride += DAV1D_PICTURE_ALIGNMENT;
    const auto luma_bytes =
        static_cast<std::size_t>(luma_stride) * static_cast<std::size_t>(aligned_height);
    const auto chroma_bytes = static_cast<std::size_t>(chroma_stride) *
                              static_cast<std::size_t>(aligned_height >> half_rows);
    auto *block = static_cast<std::uint8_t *>(
        pool->take(luma_bytes + 2 * chroma_bytes + DAV1D_PICTURE_ALIGNMENT));
    if (block == nullptr)
        return DAV1D_ERR(ENOMEM);
    picture->stride[0] = luma_stride;
    picture->stride[1] = chroma_stride;
    picture->data[0] = block;
    picture->data[1] = chroma ? block + luma_bytes : nullptr;
    picture->data[2] = chroma ? block + luma_bytes + chroma_bytes : nullptr;
    picture->allocator_data = block;
    return 0;
}

void give_picture(Dav1dPicture *picture, void *cookie)
{
    static_cast<media::PicturePool *>(cookie)->give(picture->allocator_data);
}

Av1Layout layout_of(Dav1dPixelLayout layout)
{
    switch (layout)
    {
    case DAV1D_PIXEL_LAYOUT_I400:
        return Av1Layout::i400;
    case DAV1D_PIXEL_LAYOUT_I422:
        return Av1Layout::i422;
    case DAV1D_PIXEL_LAYOUT_I444:
        return Av1Layout::i444;
    case DAV1D_PIXEL_LAYOUT_I420:
        break;
    }
    return Av1Layout::i420;
}

Av1Picture describe(const Dav1dPicture &source)
{
    Av1Picture picture;
    picture.planes.luma = static_cast<const std::uint8_t *>(source.data[0]);
    picture.planes.chroma = static_cast<const std::uint8_t *>(source.data[1]);
    picture.planes.chroma_v = static_cast<const std::uint8_t *>(source.data[2]);
    picture.planes.pitch = static_cast<std::size_t>(source.stride[0]);
    picture.planes.chroma_pitch = static_cast<std::size_t>(source.stride[1]);
    picture.planes.width = source.p.w;
    picture.planes.height = source.p.h;
    picture.planes.wide = source.p.bpc > 8;
    picture.layout = layout_of(source.p.layout);
    picture.bits = source.p.bpc;
    picture.timestamp = source.m.timestamp;
    if (source.seq_hdr != nullptr)
    {
        picture.colour.primaries = static_cast<int>(source.seq_hdr->pri);
        picture.colour.transfer = static_cast<int>(source.seq_hdr->trc);
        picture.colour.matrix = static_cast<int>(source.seq_hdr->mtrx);
        picture.colour.full_range = source.seq_hdr->color_range != 0;
    }
    if (source.content_light != nullptr)
        picture.colour.max_content_light =
            static_cast<float>(source.content_light->max_content_light_level);
    if (source.mastering_display != nullptr)
        picture.colour.mastering_peak =
            static_cast<float>(source.mastering_display->max_luminance) / 256.0f; // 24.8 fixed
    return picture;
}

} // namespace

struct Av1Decoder::Parts
{
    Dav1dContext *context = nullptr;
    std::function<void(const char *)> log;
    int error = 0; // the last error a frame thread reported, until decode() returns it

    ~Parts()
    {
        if (context != nullptr)
            dav1d_close(&context);
    }

    // One picture if one is ready. With nothing sent since the last call, dav1d waits for
    // the oldest frame in flight instead (draining), which is only wanted at the end.
    // 1: a picture went to `sink`; 0: none; -1: the sink wants no more.
    int take_one(const Sink &sink)
    {
        Dav1dPicture picture{};
        const int code = dav1d_get_picture(context, &picture);
        if (code < 0)
        {
            if (code != DAV1D_ERR(EAGAIN))
                error = code;
            return 0;
        }
        const bool more = sink(describe(picture));
        dav1d_picture_unref(&picture);
        return more ? 1 : -1;
    }
};

Av1Decoder::Av1Decoder(std::unique_ptr<Parts> parts) : parts_(std::move(parts))
{
}

Av1Decoder::~Av1Decoder() = default;

std::unique_ptr<Av1Decoder> Av1Decoder::open(const Av1Settings &settings, int *error)
{
    auto parts = std::make_unique<Parts>();
    parts->log = settings.log;
    Dav1dSettings options{};
    dav1d_default_settings(&options);
    options.n_threads = settings.threads;
    options.max_frame_delay = settings.frame_delay;
    options.frame_size_limit = settings.max_pixels;
    // A scalable stream shows its best layer only, as a player should.
    options.all_layers = 0;
    // The parts' own copy: the settings may be gone before dav1d's last message.
    options.logger.cookie = &parts->log;
    options.logger.callback = &log_line;
    if (settings.pool != nullptr)
    {
        options.allocator.cookie = settings.pool;
        options.allocator.alloc_picture_callback = &take_picture;
        options.allocator.release_picture_callback = &give_picture;
    }
    const int code = dav1d_open(&parts->context, &options);
    if (code < 0)
    {
        parts->context = nullptr;
        if (error != nullptr)
            *error = code;
        return nullptr;
    }
    return std::unique_ptr<Av1Decoder>(new Av1Decoder(std::move(parts)));
}

int Av1Decoder::decode(const std::uint8_t *data, std::size_t size, std::int64_t timestamp,
                       const Sink &sink)
{
    if (data == nullptr || size == 0)
        return 0;
    Dav1dData unit{};
    std::uint8_t *copy = dav1d_data_create(&unit, size);
    if (copy == nullptr)
        return DAV1D_ERR(ENOMEM);
    std::memcpy(copy, data, size);
    unit.m.timestamp = timestamp;
    int result = 0;
    // dav1d holds one unit at a time and gives back EAGAIN while it still has one with a
    // picture ready: take that picture, then send again.
    for (int round = 0; round < kMaxRounds; ++round)
    {
        if (unit.sz > 0)
        {
            const int code = dav1d_send_data(parts_->context, &unit);
            if (code < 0 && code != DAV1D_ERR(EAGAIN))
            {
                dav1d_data_unref(&unit);
                result = code;
            }
        }
        const int taken = parts_->take_one(sink);
        if (taken < 0)
            break;
        if (unit.sz == 0)
            break; // the next picture comes with the next unit
    }
    if (unit.sz > 0)
        dav1d_data_unref(&unit);
    if (result == 0)
        result = std::exchange(parts_->error, 0);
    parts_->error = 0;
    return result;
}

void Av1Decoder::drain(const Sink &sink)
{
    // The first call after a send only says nothing is ready; from the second on dav1d
    // waits for each frame still in flight, and has none left when it says nothing twice.
    int nothing = 0;
    while (nothing < 2)
    {
        const int taken = parts_->take_one(sink);
        if (taken < 0)
            return;
        nothing = taken == 0 ? nothing + 1 : 0;
    }
}

void Av1Decoder::flush()
{
    dav1d_flush(parts_->context);
}

std::string Av1Decoder::version()
{
    return dav1d_version();
}

} // namespace hui::ps5
