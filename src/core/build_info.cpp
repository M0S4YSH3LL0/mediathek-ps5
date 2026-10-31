// Mediathek - When this build was made, for the settings page.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/build_info.hpp"

#include <cstdio>
#include <cstring>

namespace hui
{

std::string build_time()
{
    // __DATE__ is "Oct 31 2026" (day padded with a space), __TIME__ "18:33:07": always English
    // and in the compiler's own format, so it is turned into a form that reads the same in
    // every language.
    constexpr const char *kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = __DATE__;
    const char *time = __TIME__;
    if (std::strlen(date) != 11 || std::strlen(time) != 8)
        return {};
    int month = 0;
    for (int i = 0; i < 12; ++i)
        if (std::strncmp(date, kMonths + i * 3, 3) == 0)
            month = i + 1;
    if (month == 0)
        return {};
    char text[24];
    std::snprintf(text, sizeof(text), "%.4s-%02d-%c%c %.5s", date + 7, month,
                  date[4] == ' ' ? '0' : date[4], date[5], time);
    return text;
}

} // namespace hui
