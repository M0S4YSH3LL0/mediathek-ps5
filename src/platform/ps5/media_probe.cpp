// Mediathek - Reads a movie file's headers on the console: length, codecs, languages.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/media_probe.hpp"

#include "platform/ps5/av_names.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace hui::ps5
{

namespace
{

// Only the headers and the first seconds are read: a small buffer is enough, and
// the film that may be playing at the same time keeps its memory.
constexpr int kProbeBuffer = 256 * 1024;

int read_fd(void *opaque, std::uint8_t *buffer, int size)
{
    const ssize_t got = ::read(*static_cast<int *>(opaque), buffer, static_cast<std::size_t>(size));
    if (got < 0)
        return AVERROR(errno);
    return got == 0 ? AVERROR_EOF : static_cast<int>(got);
}

std::int64_t seek_fd(void *opaque, std::int64_t offset, int whence)
{
    const int fd = *static_cast<int *>(opaque);
    if ((whence & AVSEEK_SIZE) != 0)
    {
        struct stat info = {};
        return ::fstat(fd, &info) == 0 ? static_cast<std::int64_t>(info.st_size) : -1;
    }
    const off_t at = ::lseek(fd, static_cast<off_t>(offset), whence & ~AVSEEK_FORCE);
    return at < 0 ? -1 : static_cast<std::int64_t>(at);
}

std::string container_of(const std::string &path)
{
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos)
        return std::string();
    std::string extension = path.substr(dot + 1);
    for (char &c : extension)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return extension;
}

std::string audio_format_of(const AVCodecParameters *parameters)
{
    std::string name = av::audio_name(parameters->codec_id);
    if (parameters->codec_id == AV_CODEC_ID_EAC3)
        name = "E-AC3";
    else if (parameters->codec_id == AV_CODEC_ID_TRUEHD)
        name = "TrueHD";
    const std::string layout = av::layout_name(parameters->ch_layout.nb_channels);
    return layout.empty() ? name : name + " " + layout;
}

bool probe(const std::string &path, media::MediaInfo *out)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    int descriptor = fd; // the callbacks read it through this
    auto *buffer = static_cast<std::uint8_t *>(av_malloc(kProbeBuffer));
    AVIOContext *io = buffer != nullptr ? avio_alloc_context(buffer, kProbeBuffer, 0, &descriptor,
                                                             &read_fd, nullptr, &seek_fd)
                                        : nullptr;
    AVFormatContext *format = io != nullptr ? avformat_alloc_context() : nullptr;
    const auto release = [&]()
    {
        if (format != nullptr)
            avformat_close_input(&format);
        if (io != nullptr)
        {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        else
        {
            av_free(buffer);
        }
        ::close(fd);
    };
    if (format == nullptr)
    {
        release();
        return false;
    }
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    AVDictionary *options = nullptr;
    av_dict_set(&options, "analyzeduration", "3000000", 0);
    av_dict_set(&options, "probesize", "8000000", 0);
    bool ok = avformat_open_input(&format, path.c_str(), nullptr, &options) >= 0;
    av_dict_free(&options);
    if (ok)
        ok = avformat_find_stream_info(format, nullptr) >= 0;
    if (!ok)
    {
        release();
        return false;
    }

    media::MediaInfo info;
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0)
        info.duration = static_cast<double>(format->duration) / AV_TIME_BASE;
    info.container = container_of(path);
    const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video >= 0)
    {
        const AVCodecParameters *parameters = format->streams[video]->codecpar;
        info.video_codec = av::video_name(parameters->codec_id);
        info.width = parameters->width;
        info.height = parameters->height;
    }
    const int best_audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    for (unsigned i = 0; i < format->nb_streams; ++i)
    {
        const AVStream *stream = format->streams[i];
        const AVDictionaryEntry *language = av_dict_get(stream->metadata, "language", nullptr, 0);
        const std::string code =
            media::canonical_language(language != nullptr ? language->value : "");
        // The player cannot play a track whose codec this FFmpeg build lacks (some DTS and
        // TrueHD variants), so such a track is not offered either.
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            avcodec_find_decoder(stream->codecpar->codec_id) != nullptr)
        {
            media::add_language(&info.audio_languages, code);
            if (static_cast<int>(i) == best_audio)
                info.audio_format = audio_format_of(stream->codecpar);
        }
    }
    release();
    *out = std::move(info);
    return true;
}

} // namespace

media::InfoService::Probe make_media_probe()
{
    return &probe;
}

} // namespace hui::ps5
