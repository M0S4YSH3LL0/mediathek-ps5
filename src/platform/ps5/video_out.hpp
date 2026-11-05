// Mediathek - The console's video output beside ps5-opengl: what the TV gets, and HDR10 on and off.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace hui::ps5::video_out
{

// What the output to the TV carries now: 1 SDR, 2 HDR, 0 unknown, negative when the
// output is not open or does not answer.
int dynamic_range();

// Switches the screen's buffers to HDR10 (10:10:10:2 PQ words, BT.2020) or back to 8-bit
// SDR, from the next flip on. Call between frames once the GPU has finished the last one
// (glFinish): a buffer still waiting to be shown would be shown in the new format.
// width x height is the screen's size. True when the console took it.
bool set_hdr(bool on, int width, int height);

} // namespace hui::ps5::video_out
