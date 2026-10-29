// Mediathek - Reads a movie file's headers on the console: length, codecs, languages.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/info_service.hpp"

namespace hui::ps5
{

// Opens the file with FFmpeg's demuxer and reads its stream list; nothing is
// decoded. Safe to call from a worker thread while a film plays.
media::InfoService::Probe make_media_probe();

} // namespace hui::ps5
