// Mediathek - When this build was made, for the settings page.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace hui
{

// The date and time the binary was compiled, "2026-10-31 18:33", or "" if the compiler left
// them out. The build script touches build_info.cpp on every run so the stamp is the build's.
std::string build_time();

} // namespace hui
