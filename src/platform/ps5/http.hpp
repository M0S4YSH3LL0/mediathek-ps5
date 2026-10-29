// Mediathek - Secure web requests through FFmpeg's https support.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/tmdb.hpp"

#include <string>

namespace hui::ps5
{

// A HttpGet that checks the server's certificate against `ca_file` (PEM) and
// refuses to connect when the console's random generator cannot be read.
media::HttpGet make_http_get(std::string ca_file);

} // namespace hui::ps5
