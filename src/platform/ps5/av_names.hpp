// Mediathek - Short names for FFmpeg's codecs and channel layouts, as the screens show them.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cctype>
#include <string>

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace hui::ps5::av
{

inline const char *video_name(AVCodecID id)
{
    switch (id)
    {
    case AV_CODEC_ID_H264:
        return "H.264";
    case AV_CODEC_ID_HEVC:
        return "HEVC";
    case AV_CODEC_ID_AV1:
        return "AV1";
    case AV_CODEC_ID_VP9:
        return "VP9";
    case AV_CODEC_ID_MPEG4:
        return "MPEG-4";
    case AV_CODEC_ID_MPEG2VIDEO:
        return "MPEG-2";
    default:
        return avcodec_get_name(id);
    }
}

inline std::string audio_name(AVCodecID id)
{
    std::string name = avcodec_get_name(id);
    for (char &c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

inline const char *layout_name(int channels)
{
    switch (channels)
    {
    case 1:
        return "1.0";
    case 2:
        return "2.0";
    case 6:
        return "5.1";
    case 8:
        return "7.1";
    default:
        return "";
    }
}

} // namespace hui::ps5::av
