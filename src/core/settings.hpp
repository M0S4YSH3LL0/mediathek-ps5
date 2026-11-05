// ps5-homebrew-ui - Player settings and their save format.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hui
{

// Everything the settings screen edits. The values are live: the app applies
// them the frame they change (see Shell::take_settings_changed) and stores
// them when the screen is left.
struct Settings
{
    int music_volume = 4; // 0..10
    int sfx_volume = 8;   // 0..10
    int ui_volume = 7;    // 0..10
    bool reduced_motion = false;
    bool swap_confirm = false; // Circle confirms, Cross goes back
    bool show_fps = false;
    bool haptics = true;   // controller rumble on errors and big moments
    bool light_bar = true; // the controller's light follows the design's accent
    // Mediathek: the Home billboard (art, title and Play) stays while the shelves under it
    // scroll, instead of collapsing once the first shelf is left.
    bool pin_billboard = true;
    // Mediathek: the wallpaper of the film behind its details page. Off, the page has only the
    // film's colour, as a film without a wallpaper has.
    bool details_wallpaper = true;
    // Mediathek: the language of the app's texts. 0 follows the console (or UI_LANGUAGE in
    // mediathek.env), 1 is English, 2 is German.
    int ui_language = 0;
    // Mediathek: which shelves Home shows, Continue Watching always. 0 all of them; 1 the viewer's
    // own (Recently Added, collections, All Movies); 2 genres; 3 decades.
    int home_shelves = 0;
    // Mediathek: a film with subtitle files starts with one showing, instead of none.
    bool force_subtitles = false;
    // Mediathek: the subtitle language chosen first, when subtitles are forced on or switched on.
    // 0 none (the library's order, English first), 1 German, 2 English.
    int subtitle_language = 0;
    // Mediathek: which subtitles a film offers. 0 automatic (the film's own tracks and the .srt
    // files beside it, the film's own first), 1 the film's own only, 2 the .srt files only.
    int subtitle_source = 0;
    // Display resolution, an index into kResolutions; applies at the next start.
    int resolution = 2;
    // How large on-screen controller hints are: 0 small, 1 medium, 2 large.
    int hint_size = 1;
    // The design shown when the app opens (index into the concept list).
    int concept_index = 0;

    struct Resolution
    {
        int width;
        int height;
        const char *label;
    };
    static constexpr int kResolutionCount = 3;
    static constexpr Resolution kResolutions[kResolutionCount] = {
        {1920, 1080, "1080p"}, {2560, 1440, "1440p"}, {3840, 2160, "4K"}};

    static float gain(int volume)
    {
        // Perceptual curve: 0..10 steps map to roughly -40..0 dB.
        const float t = static_cast<float>(volume) / 10.0f;
        return t * t;
    }
};

std::string encode_settings(const Settings &settings);
// Out-of-range values are clamped. Returns false (leaving settings
// untouched) for malformed data or an unknown version.
bool decode_settings(std::string_view data, Settings *settings);

} // namespace hui
