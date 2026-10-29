// Mediathek - Secure web requests through FFmpeg's https support.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/http.hpp"

#include "platform/ps5/dns.h"

extern "C"
{
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <mutex>

namespace hui::ps5
{

namespace
{

constexpr const char *kTimeoutMicroseconds = "15000000";

std::string describe(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

} // namespace

media::HttpGet make_http_get(std::string ca_file)
{
    return [ca_file = std::move(ca_file)](const media::Request &request, std::size_t max_bytes,
                                          std::string *body, std::string *error) -> bool
    {
        static std::once_flag network_ready;
        std::call_once(network_ready, [] { avformat_network_init(); });
        if (!mt_random_available())
        {
            *error = "no secure random source";
            return false;
        }
        AVDictionary *options = nullptr;
        av_dict_set(&options, "tls_verify", "1", 0);
        av_dict_set(&options, "ca_file", ca_file.c_str(), 0);
        av_dict_set(&options, "timeout", kTimeoutMicroseconds, 0);
        av_dict_set(&options, "rw_timeout", kTimeoutMicroseconds, 0);
        av_dict_set(&options, "user_agent", "Mediathek/1 (PlayStation 5)", 0);
        if (!request.headers.empty())
            av_dict_set(&options, "headers", request.headers.c_str(), 0);

        AVIOContext *io = nullptr;
        const int opened = avio_open2(&io, request.url.c_str(), AVIO_FLAG_READ, nullptr, &options);
        av_dict_free(&options);
        if (opened < 0)
        {
            *error = describe(opened);
            return false;
        }
        body->clear();
        unsigned char chunk[16384];
        bool ok = true;
        for (;;)
        {
            const int got = avio_read(io, chunk, sizeof(chunk));
            if (got == AVERROR_EOF || got == 0)
                break;
            if (got < 0)
            {
                *error = describe(got);
                ok = false;
                break;
            }
            body->append(reinterpret_cast<const char *>(chunk), static_cast<std::size_t>(got));
            if (body->size() > max_bytes)
            {
                *error = "the answer is larger than expected";
                ok = false;
                break;
            }
        }
        avio_closep(&io);
        return ok;
    };
}

} // namespace hui::ps5
