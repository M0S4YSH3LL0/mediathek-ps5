// Mediathek - Build stamp tests.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/build_info.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <string>

TEST(BuildInfo, TheBuildTimeIsADateAndAMinute)
{
    const std::string stamp = hui::build_time();
    ASSERT_EQ(stamp.size(), 16u); // 2026-10-31 18:33
    for (std::size_t i = 0; i < stamp.size(); ++i)
    {
        const bool separator = i == 4 || i == 7 || i == 10 || i == 13;
        if (separator)
            EXPECT_EQ(stamp[i], i == 10 ? ' ' : (i == 13 ? ':' : '-')) << stamp;
        else
            EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(stamp[i])) != 0) << stamp;
    }
}
