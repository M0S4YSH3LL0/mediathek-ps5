// Mediathek - The console's direct memory as blocks for decoded pictures.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "media/picture_pool.hpp"

namespace hui::ps5
{

// Blocks of direct memory, the pool the hardware decoder's pictures come from too: the
// app's heap is fixed and its flexible memory spent on it, and a 4K AV1 film's reference
// pictures alone take some 250 MB. When the system has no more, a block comes from the heap
// instead, but only while plenty of it stays free (running the heap out ends the app).
media::PicturePool::Backend direct_picture_memory();

} // namespace hui::ps5
