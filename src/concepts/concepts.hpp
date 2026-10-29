// Mediathek - Factories of the app's screens.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/concept.hpp"

#include <memory>

namespace hui::concepts
{

// One function per design, each defined in its own file in this directory.
std::unique_ptr<app::Concept> make_mediathek(app::Context &context);

} // namespace hui::concepts
