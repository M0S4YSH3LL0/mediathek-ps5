// Mediathek - How the player drops frames when the picture decoder cannot keep up.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace hui::media
{

// What the decoder should leave out of the packet it is about to get.
enum class Skip
{
    none,
    non_reference, // frames nothing else is predicted from (B-frames, mostly)
    to_key_frame   // everything up to the next key frame near the sound
};

// A decoder on one slow thread cannot show a 4K film at 60 frames a second. The
// sound does not wait for it, so the picture has to: first by not decoding the
// frames that can be left out, and when it is far behind, by jumping ahead to a
// key frame, as video players do. Used by the decoding thread only.
class DecodePace
{
  public:
    // `lag` is how many seconds the packet's picture is behind the sound (negative:
    // ahead of it), `key` whether the packet is a key frame. `arrived` is false from a
    // start or a seek until its first picture is out: a seek lands on the key frame
    // before its target, so the decoder's way from there to the target looks like
    // seconds of lag. Those packets are the way there, and skipping them would leave
    // the picture standing until the key frame after the target.
    Skip next(double lag, bool key, bool arrived = true);

    // Leaving frames out only helps a decoder on one thread. With frame threads the
    // frames nothing depends on are what runs alongside the next one: a 4K sample
    // decodes at 32 fps without them on any number of threads, at 47 fps with them
    // on one and 75 on two. Jumping stays.
    void allow_thinning(bool allowed)
    {
        thinning_allowed_ = allowed;
        if (!allowed)
            thinning_ = false;
    }

    // A seek starts over; what the threads can do stays as it was set.
    void reset();

  private:
    bool thinning_allowed_ = true;
    bool thinning_ = false; // sticky: a decoder too slow once still is
    bool jumping_ = false;
};

} // namespace hui::media
