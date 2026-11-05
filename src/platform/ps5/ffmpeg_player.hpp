// Mediathek - Movie playback on the console: FFmpeg decodes, the app shows and plays.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "audio/stream_ring.hpp"
#include "media/player.hpp"

#include <functional>
#include <memory>

namespace hui::ps5
{

// Software decoding (PacBrew's FFmpeg) on worker threads: one reads the file,
// one decodes pictures, one decodes sound into `ring`, which the mixer plays.
// The sound is the clock: pictures are shown when the sound has reached them.
// `ring` must outlive the player and be attached to a mixer stream slot.
// `sound` switches that slot on and off: pausing mutes it, because the mixer
// keeps playing what is already buffered (up to a few seconds) after decoding
// has stopped. `hardware` sends every H.264 and HEVC film it can take to the console's
// picture decoder, with FFmpeg still reading the file and decoding the sound, and
// decoding the pictures when the hardware refuses or gives up (HW_DECODE=0 says no).
std::unique_ptr<media::Player> make_ffmpeg_player(const media::VideoSurface &surface,
                                                  audio::StreamRing *ring,
                                                  std::function<void(bool)> sound, bool hardware);

} // namespace hui::ps5
