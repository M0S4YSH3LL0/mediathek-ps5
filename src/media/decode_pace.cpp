// Mediathek - How the player drops frames when the picture decoder cannot keep up.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "media/decode_pace.hpp"

namespace hui::media
{

namespace
{

constexpr double kBehind = 0.25; // seconds: thin the frames out (the player drops pictures at 0.3)
constexpr double kLost = 0.8;    // seconds: nothing cheaper will catch up, jump
constexpr double kNear = 0.3;    // seconds: a key frame this close to the sound ends the jump

} // namespace

Skip DecodePace::next(double lag, bool key, bool arrived)
{
    if (!arrived)
        return Skip::none;
    if (jumping_)
    {
        if (!key || lag >= kNear)
            return Skip::to_key_frame;
        jumping_ = false;
    }
    else if (lag > kLost)
    {
        thinning_ = thinning_allowed_; // a decoder this far behind needs the cheaper frames too
        jumping_ = true;
        return Skip::to_key_frame;
    }
    else if (lag > kBehind && thinning_allowed_)
    {
        thinning_ = true;
    }
    return thinning_ ? Skip::non_reference : Skip::none;
}

void DecodePace::reset()
{
    thinning_ = false;
    jumping_ = false;
}

} // namespace hui::media
