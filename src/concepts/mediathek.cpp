// Mediathek - The movie library screen: hero billboard, shelves, grid, details and player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A living-room movie browser in the manner of the big streaming apps:
//
//   - a billboard for the focused movie (its fanart, or its poster when the
//     folder has none) above horizontal shelves of posters: Continue Watching,
//     Recently Added, collections, decades, everything A to Z;
//   - the page takes its tint from the focused poster, eased, and the billboard
//     collapses to a slim header once the focus leaves the first shelf;
//   - a second tab lays every movie out as a grid with sorting;
//   - Cross opens a full-screen details page (play, resume, start over, watched),
//     Square plays at once;
//   - the player is part of the screen: a black stage with a fading on-screen
//     display, scrubber, audio track and subtitle switching, and resume points
//     that are saved as the movie plays.
//
// Posters are decoded on a worker thread and fade in when they arrive; a movie
// without art gets a generated cover from its title.

#include "concepts/concepts.hpp"

#include "core/build_info.hpp"
#include "core/i18n.hpp"
#include "core/save_file.hpp"
#include "core/tween.hpp"
#include "media/library.hpp"
#include "media/mediainfo.hpp"
#include "media/services.hpp"
#include "media/subtitles.hpp"
#include "ui/components/dialog.hpp"
#include "ui/components/form.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/components/list.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;
using i18n::Text;
using i18n::tr; // the text in the language of the console

const Color kInk = Color::rgb(0xf5f5f7);
const Color kBlack = Color::rgb(0x07070b);
const Color kAccent = Color::rgb(0xff4d5e);

constexpr float kMargin = 96.0f;
constexpr float kGearRadius = 17.0f;
constexpr int kTabCount = 3; // Tab::home .. Tab::settings
constexpr float kExitConfirmSeconds = 2.5f;
constexpr int kSearchLength = 40;

// The rows of the settings page; the form hands these back when one changes.
enum SettingsRow : int
{
    kRowUiSounds = 1,
    kRowPinBillboard,
    kRowDetailsWallpaper,
    kRowHomeShelves,
    kRowLanguage,
    kRowReload,
    kRowDebug,
    kRowVersion,
};

// What Settings::home_shelves holds, in the order of the row's options.
enum HomeShelves : int
{
    kShelvesAll,
    kShelvesOwn,
    kShelvesGenres,
    kShelvesDecades,
};

// The list on the left of the settings page, top to bottom; its focus is one of these.
enum SettingsCategory : int
{
    kCategorySound,
    kCategoryAppearance,
    kCategoryLanguage,
    kCategoryLibrary,
    kCategoryDeveloper,
};
constexpr float kSettingsTop = 244.0f;
constexpr float kSettingsHeight = 712.0f; // ends above the hint row
constexpr float kSettingsListWidth = 440.0f;
constexpr float kSettingsFormLeft = 584.0f;
constexpr float kSettingsRight = 1824.0f;
constexpr float kCardW = 176.0f; // poster at rest on a shelf
constexpr float kCardH = 264.0f;
constexpr float kCardGap = 20.0f;
constexpr float kCardGrow = 1.14f;
constexpr float kRowPitch = 372.0f;
constexpr float kTopFull = 700.0f;    // top of the focused shelf's posters, billboard open
constexpr float kTopCompact = 440.0f; // ... and with the billboard collapsed

constexpr int kGridColumns = 8;
constexpr float kGridW = 190.0f;
constexpr float kGridH = 285.0f;
constexpr float kGridPitchX = 218.0f;
constexpr float kGridPitchY = 332.0f;
constexpr float kGridTop = 232.0f;
// Cards scrolling up under the "All Movies" header fade out above this line.
constexpr float kGridFadeTop = kGridTop - 34.0f;

constexpr const char *kAppVersion = "0.6.4"; // equals contentVersion 01.000.064 in param.json
constexpr float kOsdSeconds = 4.0f;
constexpr float kSeekHoldGap = 0.5f;   // a pause this long in the repeats ends a hold
constexpr double kSeekHeldStep = 30.0; // first held step, and what each held second adds
constexpr double kSeekHeldMax = 300.0; // the longest step, 5 minutes

// Seconds to jump for a direction held `held` seconds: 30, 60, 90 ... up to 5 minutes.
double held_seek_step(float held)
{
    return std::min(kSeekHeldMax, kSeekHeldStep * (1.0 + std::floor(static_cast<double>(held))));
}
constexpr float kSaveEvery = 5.0f;

constexpr const char *kTechniques[] = {
    "Billboard and page tint follow the focused poster (ui::SpringColor)",
    "Posters decode on a worker thread and fade in; missing art gets a generated cover",
    "Shelves for continue watching, new, collections, decades; a sortable grid",
    "Full-screen details page; resume points saved while a movie plays",
    "Player stage with scrubber, audio tracks and SubRip subtitles",
};

// A picture shows the state before its own step's input acts, so each named
// step follows the step that caused what it shows.
constexpr app::TourStep kTour[] = {
    {0.9f, 0, Direction::right},
    {0.25f, 0, Direction::right},
    {0.9f, 0, Direction::none, "home"},
    {0.2f, 0, Direction::down},
    {0.2f, 0, Direction::right},
    {0.2f, 0, Direction::right},
    {1.0f, 0, Direction::down},
    {1.0f, 0, Direction::none, "shelves"},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::down},
    {1.2f, 0, Direction::none, "genres"},
    {0.3f, 0, Direction::up},
    {0.3f, 0, Direction::up},
    {0.8f, 0, Direction::none},
    {0.2f, action_bit(Action::confirm), Direction::none},
    {0.8f, 0, Direction::none, "details"},
    {0.2f, action_bit(Action::page_next)},
    {0.8f, 0, Direction::none, "details-next"},
    {0.2f, action_bit(Action::back)},
    {0.4f, action_bit(Action::page_next)},
    {1.0f, 0, Direction::none, "grid"},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::right},
    {0.3f, 0, Direction::right},
    {1.0f, 0, Direction::down},
    {1.0f, 0, Direction::none, "movies"},
    {0.2f, action_bit(Action::north)},
    {0.8f, 0, Direction::none, "search"},
    {0.2f, action_bit(Action::confirm)},
    {0.4f, action_bit(Action::menu)},
    {0.8f, 0, Direction::none, "search-results"},
    {0.2f, action_bit(Action::back)},
    {0.2f, action_bit(Action::west)},
    {1.6f, 0, Direction::none, "playing"},
    {0.2f, 0, Direction::right},
    {0.9f, 0, Direction::none, "seek"},
    {0.2f, action_bit(Action::confirm)},
    {0.8f, 0, Direction::none, "paused"},
    {0.4f, action_bit(Action::back)},
    {0.4f, action_bit(Action::page_next)},
    {1.0f, 0, Direction::none, "settings"},
    {0.3f, 0, Direction::right},
    {0.2f, action_bit(Action::confirm)},
    {0.9f, 0, Direction::none, "settings-changed"},
    {0.3f, action_bit(Action::back)},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::right},
    {0.3f, 0, Direction::down},
    {0.9f, 0, Direction::none, "settings-appearance"},
    {0.3f, 0, Direction::down},
    {0.9f, 0, Direction::none, "settings-shelves"},
    {0.3f, action_bit(Action::back)},
    {0.3f, 0, Direction::down},
    {0.9f, 0, Direction::none, "settings-language"},
    {0.3f, 0, Direction::down},
    {0.3f, action_bit(Action::confirm)},
    {0.2f, action_bit(Action::confirm)},
    {0.9f, 0, Direction::none, "reload"},
    {0.3f, action_bit(Action::back)},
    {0.3f, action_bit(Action::back)},
    {0.3f, 0, Direction::down},
    {0.3f, 0, Direction::right},
    {0.9f, 0, Direction::none, "settings-developer"},
    {0.4f, action_bit(Action::page_prev)},
    {0.8f, action_bit(Action::page_prev)},
};

enum class Screen : std::uint8_t
{
    browse,
    details,
    player,
};

enum class Tab : std::uint8_t
{
    home,
    movies,
    settings, // the gear at the top right
};

enum class Sort : std::uint8_t
{
    title,
    newest,
    year,
    count,
};

enum class SheetAction : std::uint8_t
{
    play,
    restart,
    watched,
    cover,
    close,
};

struct Shelf
{
    std::string title;
    std::vector<int> movies; // library indices
    int column = 0;
    ui::Scroller scroll;
};

float hash01(const std::string &text, std::uint32_t salt)
{
    std::uint32_t h = 2166136261u ^ salt;
    for (const char c : text)
        h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
    return static_cast<float>(h % 10007u) / 10007.0f;
}

Color hsv(float h, float s, float v)
{
    h = h - std::floor(h);
    const float k = h * 6.0f;
    const int i = static_cast<int>(k);
    const float f = k - static_cast<float>(i);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    switch (i % 6)
    {
    case 0:
        return {v, t, p, 1.0f};
    case 1:
        return {q, v, p, 1.0f};
    case 2:
        return {p, v, t, 1.0f};
    case 3:
        return {p, q, v, 1.0f};
    case 4:
        return {t, p, v, 1.0f};
    default:
        return {v, p, q, 1.0f};
    }
}

// Texture rectangle that fills `aspect` (width / height) from an image of
// the given size, cropping the overflow equally on both sides.
Rect cover_uv(float width, float height, float aspect)
{
    const float image = width / std::max(1.0f, height);
    if (image > aspect)
    {
        const float u = aspect / image;
        return {(1.0f - u) * 0.5f, 0.0f, u, 1.0f};
    }
    const float v = image / aspect;
    return {0.0f, (1.0f - v) * 0.5f, 1.0f, v};
}

std::string format_size(std::uint64_t bytes)
{
    char text[32];
    if (bytes >= (1ull << 30))
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1ull << 30));
    else
        std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / (1ull << 20));
    return text;
}

std::string format_time(double seconds)
{
    const int total = std::max(0, static_cast<int>(seconds));
    char text[32];
    if (total >= 3600)
        std::snprintf(text, sizeof(text), "%d:%02d:%02d", total / 3600, total / 60 % 60,
                      total % 60);
    else
        std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}

std::string meta_line(const media::Movie &movie)
{
    std::string line;
    const auto add = [&](const std::string &part)
    {
        if (part.empty())
            return;
        if (!line.empty())
            line += "  \xC2\xB7  ";
        line += part;
    };
    if (movie.year > 0)
        add(std::to_string(movie.year));
    add(movie.quality);
    if (movie.size > 0)
        add(format_size(movie.size));
    add(movie.drive);
    return line;
}

class Mediathek final : public app::Concept
{
  public:
    explicit Mediathek(app::Context &context)
        : context_(context), svc_(context.media != nullptr ? context.media : &fallback_)
    {
        rebuild();
        snap_layout();
        setup_search();
        setup_settings();
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "mediathek",
            "Mediathek",
            "Movies from the console's drives, shelved like a streaming service",
            "src/concepts/mediathek.cpp",
            audio::SoundSet::glass,
            kAccent,
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        clock_ += dt;
        exit_armed_ = std::max(0.0f, exit_armed_ - dt);
        if (revision_ != svc_->revision)
            rebuild();
        if (svc_->cover_note != cover_note_)
        {
            cover_note_ = svc_->cover_note;
            if (!cover_note_.empty())
            {
                toast_ = cover_note_;
                toast_time_ = 4.5f;
            }
        }

        if (svc_->scan_found >= 0)
        {
            // The small message at the bottom once a scan has finished, after "Looking for
            // movies...". It is the app's own: the helper payload cannot post one of its own
            // that fits the app.
            notify(feedback,
                   i18n::format(Text::scan_found,
                                i18n::movie_count(static_cast<std::size_t>(svc_->scan_found))),
                   audio::Cue::notify);
            svc_->scan_found = -1;
        }
        update_reload_progress(feedback);

        switch (screen_)
        {
        case Screen::browse:
            update_browse(input, feedback);
            break;
        case Screen::details:
            update_details(input, feedback);
            break;
        case Screen::player:
            update_player(input, dt, feedback);
            break;
        }
        animate(dt);
    }

    void draw(app::Frame &frame) const override
    {
        if (screen_ == Screen::player)
        {
            frame.backdrop.mode = gfx::BackdropMode::none;
            draw_player(frame.scene);
            return;
        }
        draw_page(frame);
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

    // ---- for tests ----
    int focused_movie_for_test() const
    {
        return focused_movie();
    }
    bool playing_for_test() const
    {
        return screen_ == Screen::player;
    }
    bool details_open_for_test() const
    {
        return screen_ == Screen::details;
    }

  private:
    // ---------------------------------------------------------------- data

    const media::Library &library() const
    {
        return svc_->library;
    }
    const media::Movie &movie(int index) const
    {
        return svc_->library[static_cast<std::size_t>(index)];
    }

    int focused_movie() const
    {
        if (tab_ == Tab::settings)
            return -1;
        if (tab_ == Tab::home)
        {
            if (shelves_.empty())
                return -1;
            const Shelf &shelf = shelves_[static_cast<std::size_t>(row_)];
            return shelf.movies.empty() ? -1 : shelf.movies[static_cast<std::size_t>(shelf.column)];
        }
        return grid_.empty() ? -1 : grid_[static_cast<std::size_t>(grid_focus_)];
    }

    void rebuild()
    {
        revision_ = svc_->revision;
        const std::string keep = focus_id_;
        shelves_.clear();
        const media::Library &lib = library();
        const int total = static_cast<int>(lib.size());
        const int shown = context_.settings.home_shelves;
        const bool own = shown == kShelvesAll || shown == kShelvesOwn;
        const bool genres = shown == kShelvesAll || shown == kShelvesGenres;
        const bool decade_shelves = shown == kShelvesAll || shown == kShelvesDecades;

        const auto add = [&](std::string title, std::vector<int> movies)
        {
            if (movies.empty())
                return;
            Shelf shelf;
            shelf.title = std::move(title);
            shelf.movies = std::move(movies);
            shelves_.push_back(std::move(shelf));
        };

        std::vector<int> continuing;
        for (const media::Watch *watch : svc_->progress.in_progress_recent())
        {
            const int index = lib.find(watch->id);
            if (index >= 0 && continuing.size() < 20)
                continuing.push_back(index);
        }
        // Every choice keeps it: it is where the viewer left off, not a way to sort the library.
        add(tr(Text::shelf_continue), continuing);
        const std::size_t always = shelves_.size();

        std::vector<int> all(static_cast<std::size_t>(total));
        for (int i = 0; i < total; ++i)
            all[static_cast<std::size_t>(i)] = i;

        if (own && total >= 8)
        {
            std::vector<int> recent = all;
            std::stable_sort(recent.begin(), recent.end(),
                             [&](int a, int b) { return movie(a).added > movie(b).added; });
            recent.resize(std::min<std::size_t>(recent.size(), 24));
            add(tr(Text::shelf_recent), recent);
        }

        std::map<std::string, std::vector<int>> collections;
        std::map<int, std::vector<int>, std::greater<int>> decades;
        for (const int i : all)
        {
            if (!movie(i).collection.empty())
                collections[movie(i).collection].push_back(i);
            if (movie(i).year > 0)
                decades[movie(i).year / 10 * 10].push_back(i);
        }
        if (own)
            for (auto &entry : collections)
                if (entry.second.size() >= 2)
                    add(entry.first, entry.second);
        // One shelf for every genre a film has among its two main ones (the two the details
        // page names), the fullest first.
        if (genres)
            for (media::GenreShelf &entry : media::genre_shelves(lib.movies()))
                add(media::genre_name(entry.genre), std::move(entry.movies));
        if (decade_shelves && total >= 12)
            for (auto &entry : decades)
                if (entry.second.size() >= 3)
                    add(i18n::format(Text::decade, std::to_string(entry.first)), entry.second);

        // Without covers there are no genres, and a small library has no decades: the films must
        // still be on Home, and the rest of the page counts on a shelf while there are films.
        if (own || shelves_.size() == always)
            add(tr(total >= 8 ? Text::shelf_all : Text::shelf_yours), all);

        row_ = 0;
        if (!keep.empty())
        {
            // Back to the same movie, in the first shelf that holds it.
            const int wanted = lib.find(keep);
            for (int r = 0; wanted >= 0 && r < static_cast<int>(shelves_.size()); ++r)
            {
                const auto &movies = shelves_[static_cast<std::size_t>(r)].movies;
                const auto it = std::find(movies.begin(), movies.end(), wanted);
                if (it != movies.end())
                {
                    row_ = r;
                    shelves_[static_cast<std::size_t>(r)].column =
                        static_cast<int>(it - movies.begin());
                    break;
                }
            }
        }
        rebuild_grid(keep);
        previous_ = shown_ = focused_movie();
    }

    void rebuild_grid(const std::string &keep)
    {
        const media::Library &lib = library();
        grid_.clear();
        for (std::size_t i = 0; i < lib.size(); ++i)
            if (media::matches_search(lib[i], search_))
                grid_.push_back(static_cast<int>(i));
        std::stable_sort(grid_.begin(), grid_.end(),
                         [&](int a, int b)
                         {
                             switch (sort_)
                             {
                             case Sort::newest:
                                 return movie(a).added > movie(b).added;
                             case Sort::year:
                                 return movie(a).year != movie(b).year
                                            ? movie(a).year > movie(b).year
                                            : false;
                             default:
                                 return false; // the library is already A to Z
                             }
                         });
        grid_focus_ = 0;
        const int wanted = keep.empty() ? -1 : lib.find(keep);
        for (std::size_t i = 0; wanted >= 0 && i < grid_.size(); ++i)
            if (grid_[i] == wanted)
                grid_focus_ = static_cast<int>(i);
    }

    // ------------------------------------------------------------- update

    void notify(app::Feedback &feedback, const std::string &text, audio::Cue cue)
    {
        toast_ = text;
        toast_time_ = 2.6f;
        feedback.play(cue);
    }

    void refuse(app::Feedback &feedback, const InputFrame &input)
    {
        if (input.nav_repeat)
            return;
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        feedback.rumble(0.25f, 0.05f);
        nudge_.trigger();
    }

    void update_browse(const InputFrame &input, app::Feedback &feedback)
    {
        if (search_prompt_.is_open())
        {
            update_search(input, feedback);
            return;
        }
        if (reload_dialog_.is_open())
        {
            update_settings(input, feedback);
            return;
        }
        if (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev))
        {
            // Home, Movies, Settings: L1 and R1 walk along them and stop at the ends.
            const int step = input.is_pressed(Action::page_next) ? 1 : -1;
            const int to = std::clamp(static_cast<int>(tab_) + step, 0, kTabCount - 1);
            if (to != static_cast<int>(tab_))
            {
                tab_ = static_cast<Tab>(to);
                tab_age_ = 0.0f;
                if (tab_ == Tab::settings)
                    open_settings();
                feedback.play(audio::Cue::tab, tab_ == Tab::home ? 0.94f : 1.08f);
            }
        }
        if (input.is_pressed(Action::back) && handle_back(feedback))
            return;
        if (input.is_pressed(Action::menu) && svc_->rescan && !svc_->scanning)
        {
            svc_->rescan();
            notify(feedback, tr(Text::looking_for_movies), audio::Cue::notify);
        }

        if (tab_ == Tab::settings)
        {
            update_settings(input, feedback);
            return;
        }

        // Search and sort also work on an empty result: that is how it is changed.
        if (tab_ == Tab::movies && library().size() > 0)
        {
            if (input.is_pressed(Action::north))
            {
                search_prompt_.open(feedback, search_);
                return;
            }
            if (input.is_pressed(Action::r3))
            {
                sort_ = static_cast<Sort>((static_cast<int>(sort_) + 1) %
                                          static_cast<int>(Sort::count));
                rebuild_grid(focus_id_);
                notify(feedback, i18n::format(Text::sorted_by, sort_name()), audio::Cue::tab);
            }
        }

        const int focused = focused_movie();
        if (focused < 0)
            return; // nothing to move over

        if (tab_ == Tab::home)
            update_shelves(input, feedback);
        else
            update_grid(input, feedback);

        if (input.is_pressed(Action::confirm))
        {
            open_details(feedback);
        }
        else if (input.is_pressed(Action::west))
        {
            start_playback(focused, true, feedback);
        }
        else if (input.is_pressed(Action::north) && tab_ == Tab::home)
        {
            toggle_watched(focused, feedback);
        }
    }

    // Back steps out one level at a time: a search, Movies to Home, then to the first
    // film, then (asking twice, so a stray press does not close the app) out.
    // True when the press was used.
    bool handle_back(app::Feedback &feedback)
    {
        if (tab_ == Tab::settings && settings_in_form_)
        {
            focus_settings_form(false);
            feedback.play(audio::Cue::back);
            return true;
        }
        if (tab_ == Tab::movies && !search_.empty())
        {
            search_.clear();
            rebuild_grid(focus_id_);
            feedback.play(audio::Cue::back);
            return true;
        }
        if (tab_ != Tab::home)
        {
            tab_ = Tab::home;
            tab_age_ = 0.0f;
            feedback.play(audio::Cue::back);
            return true;
        }
        if (!shelves_.empty() && (row_ != 0 || shelves_[0].column != 0))
        {
            row_ = 0;
            shelves_[0].column = 0;
            feedback.play(audio::Cue::back);
            return true;
        }
        if (!svc_->quit)
            return false;
        if (exit_armed_ > 0.0f)
        {
            svc_->quit();
            return true;
        }
        exit_armed_ = kExitConfirmSeconds;
        notify(feedback, tr(Text::press_back_again), audio::Cue::notify);
        toast_time_ = kExitConfirmSeconds;
        return true;
    }

    // Triangle on the Movies page: the on-screen keyboard. Done with an empty line clears
    // the search, and so does Back once.
    void update_search(const InputFrame &input, app::Feedback &feedback)
    {
        if (search_prompt_.handle(input, feedback) != ui::Event::activated)
            return;
        search_ = search_prompt_.text();
        rebuild_grid("");
    }

    // The kit's components in the screen's own colours.
    static ui::Theme app_theme()
    {
        ui::Theme theme = ui::themes()[0];
        theme.page = Color::rgb(0x0e0f16);
        theme.surface = Color::rgb(0x15161f);
        theme.surface_high = Color::rgb(0x242633);
        theme.primary = kAccent;
        theme.accent = kAccent;
        theme.on_primary = kInk;
        theme.focus = kInk;
        return theme;
    }

    void setup_search()
    {
        search_prompt_.style.theme = app_theme();
        search_prompt_.style.max_length = kSearchLength;
        search_prompt_.style.allow_empty = true;
        search_prompt_.style.frosted = false;
        search_prompt_.style.reduced_motion = context_.settings.reduced_motion;
        search_prompt_.style.done_label = tr(Text::search_do);
        search_prompt_.style.cancel_label = tr(Text::search_cancel);
        search_prompt_.set_title(tr(Text::search_title));
        search_prompt_.field.set_placeholder(tr(Text::search_placeholder));
        // Triangle is a space and Options is Done, as everywhere on the console.
        search_prompt_.keyboard.style.bindings = ui::KeyboardBindings::standard();
        search_prompt_.keyboard.style.bindings.done = Action::menu;
        search_prompt_.keyboard.style.space_label = tr(Text::search_space);
        search_prompt_.keyboard.style.done_label = tr(Text::search_do);
    }

    // The settings page is a list of categories on the left and a ui::Form with the settings of
    // the chosen one on the right: every row is applied and saved the moment it changes (the
    // shell stores it a moment after the last change).
    void setup_settings()
    {
        const bool calm = context_.settings.reduced_motion;
        settings_form_.style.theme = app_theme();
        settings_form_.style.reduced_motion = calm;
        settings_form_.style.control_width = 360.0f;
        settings_form_.style.on_text = tr(Text::settings_on);
        settings_form_.style.off_text = tr(Text::settings_off);
        settings_form_.style.idle_focus = false; // while the list has the focus, only it shows one
        settings_form_.set_bounds(
            {kSettingsFormLeft, kSettingsTop, kSettingsRight - kSettingsFormLeft, kSettingsHeight});

        settings_list_.style.theme = app_theme();
        settings_list_.style.reduced_motion = calm;
        settings_list_.style.row_height = settings_form_.style.row_height;
        settings_list_.style.title_size = settings_form_.style.label_size;
        settings_list_.set_bounds({kMargin, kSettingsTop, kSettingsListWidth, kSettingsHeight});
        std::vector<ui::ListItem> categories(5);
        categories[kCategorySound].title = tr(Text::settings_sound);
        categories[kCategoryAppearance].title = tr(Text::settings_appearance);
        categories[kCategoryLanguage].title = tr(Text::settings_language);
        categories[kCategoryLibrary].title = tr(Text::settings_library);
        categories[kCategoryDeveloper].title = tr(Text::settings_developer);
        settings_list_.set_items(std::move(categories));
        build_settings_form();

        reload_dialog_.style.theme = app_theme();
        reload_dialog_.style.frosted = false;
        reload_dialog_.style.reduced_motion = calm;
    }

    // The rows of the category the list has its focus on.
    void build_settings_form()
    {
        settings_form_.clear();
        switch (settings_list_.focus())
        {
        case kCategorySound:
            settings_form_.add_toggle(kRowUiSounds, tr(Text::settings_ui_sounds)).description =
                tr(Text::settings_ui_sounds_hint);
            break;
        case kCategoryAppearance:
            settings_form_.add_toggle(kRowPinBillboard, tr(Text::settings_pin_billboard))
                .description = tr(Text::settings_pin_billboard_hint);
            settings_form_.add_toggle(kRowDetailsWallpaper, tr(Text::settings_details_wallpaper))
                .description = tr(Text::settings_details_wallpaper_hint);
            settings_form_
                .add_choice(kRowHomeShelves, tr(Text::settings_home_shelves),
                            {tr(Text::settings_home_shelves_all),
                             tr(Text::settings_home_shelves_own),
                             tr(Text::settings_home_shelves_genres),
                             tr(Text::settings_home_shelves_decades)})
                .description = tr(Text::settings_home_shelves_hint);
            break;
        case kCategoryLanguage:
            settings_form_.add_choice(kRowLanguage, tr(Text::settings_ui_language),
                                      {tr(Text::settings_language_auto), "English", "Deutsch"});
            break;
        case kCategoryLibrary:
            settings_form_.add_action(kRowReload, tr(Text::settings_reload)).description =
                tr(Text::settings_reload_hint);
            break;
        default:
            settings_form_.add_toggle(kRowDebug, tr(Text::settings_debug)).description =
                tr(Text::settings_debug_hint);
            // Says which build this is: whether the console runs the one you just made.
            settings_form_.add_value(kRowVersion, tr(Text::settings_version), version_text());
            break;
        }
        sync_settings_form();
        settings_form_.set_focus(0, true);
        settings_form_.enter();
    }

    static std::string version_text()
    {
        std::string text = std::string("v") + kAppVersion;
        if (const std::string stamp = build_time(); !stamp.empty())
            text += std::string("  \xC2\xB7  ") + tr(Text::settings_built) + " " + stamp;
        return text;
    }

    // The form shows what the settings hold now (they may have been loaded after it was built).
    void sync_settings_form()
    {
        settings_form_.set_toggle(kRowUiSounds, context_.settings.ui_volume > 0);
        settings_form_.set_toggle(kRowPinBillboard, context_.settings.pin_billboard);
        settings_form_.set_toggle(kRowDetailsWallpaper, context_.settings.details_wallpaper);
        settings_form_.set_toggle(kRowDebug, context_.settings.show_fps);
        settings_form_.set_choice(kRowHomeShelves, context_.settings.home_shelves);
        settings_form_.set_choice(kRowLanguage, context_.settings.ui_language);
    }

    void open_settings()
    {
        focus_settings_form(false);
        settings_list_.set_focus(kCategorySound, true);
        settings_list_.enter();
        build_settings_form();
    }

    void focus_settings_form(bool in_form)
    {
        settings_in_form_ = in_form;
        settings_held_ = in_form;
        settings_list_.set_active(!in_form);
        settings_form_.set_active(in_form);
    }

    void update_settings(const InputFrame &input, app::Feedback &feedback)
    {
        if (reload_dialog_.is_open())
        {
            if (reload_dialog_.handle(input, feedback) == ui::Event::activated &&
                reload_dialog_.choice() == 1)
                reload_everything(feedback);
            return;
        }
        const bool sideways = input.nav == Direction::left || input.nav == Direction::right;
        if (!settings_in_form_)
        {
            if (input.nav == Direction::right && !input.nav_repeat)
            {
                focus_settings_form(true);
                feedback.play(audio::Cue::focus);
                return;
            }
            const ui::Event moved = settings_list_.handle(input, feedback);
            if (moved == ui::Event::moved)
                build_settings_form();
            else if (moved == ui::Event::activated)
                focus_settings_form(true);
            return;
        }
        // Right into the form may still be held: its repeats must not go on to change a choice.
        if (input.nav != Direction::none && !input.nav_repeat)
            settings_held_ = false;
        if (sideways)
        {
            if (settings_held_ && input.nav_repeat)
                return;
            // Only a choice uses left and right; everywhere else left is the way back to the
            // list, so a switch is changed with confirm alone and left never changes it by
            // accident.
            const ui::FormRow *row = settings_form_.row(settings_form_.focus_id());
            if (row == nullptr || row->kind != ui::FormRowKind::choice)
            {
                if (input.nav == Direction::left && !input.nav_repeat)
                {
                    focus_settings_form(false);
                    feedback.play(audio::Cue::back);
                }
                return;
            }
        }
        const ui::Event event = settings_form_.handle(input, feedback);
        if (event == ui::Event::changed)
            apply_setting(settings_form_.changed_id());
        else if (event == ui::Event::activated && settings_form_.changed_id() == kRowReload)
            ask_reload(feedback);
    }

    // The texts are looked up as they are drawn, but some were put into components and shelves
    // when those were built: build them again in the new language, and stay on the same row.
    void change_language()
    {
        i18n::set_language(
            i18n::choose_language(context_.settings.ui_language, svc_->automatic_language));
        rebuild();
        setup_search();
        setup_settings();
        settings_form_.focus_row(kRowLanguage, true);
    }

    void apply_setting(int row)
    {
        Settings &settings = context_.settings;
        if (row == kRowUiSounds)
        {
            // Off is volume 0; on is the volume the app starts with.
            settings.ui_volume = settings_form_.toggle_value(row) ? Settings{}.ui_volume : 0;
        }
        else if (row == kRowPinBillboard)
        {
            settings.pin_billboard = settings_form_.toggle_value(row);
        }
        else if (row == kRowDetailsWallpaper)
        {
            settings.details_wallpaper = settings_form_.toggle_value(row);
        }
        else if (row == kRowHomeShelves)
        {
            settings.home_shelves = settings_form_.choice_index(row);
            rebuild();
        }
        else if (row == kRowLanguage)
        {
            settings.ui_language = settings_form_.choice_index(row);
            change_language();
        }
        else if (row == kRowDebug)
        {
            settings.show_fps = settings_form_.toggle_value(row);
        }
        else
        {
            return;
        }
        context_.settings_changed = true; // the app applies it, the shell saves it
    }

    void ask_reload(app::Feedback &feedback)
    {
        if (library().size() == 0)
        {
            notify(feedback, tr(Text::reload_nothing), audio::Cue::error);
            return;
        }
        ui::DialogContent content;
        content.icon = ui::StatusKind::question;
        content.title = tr(Text::reload_title);
        content.body = i18n::format(Text::reload_body, std::to_string(library().size()));
        content.buttons = {{tr(Text::reload_cancel)},
                           {tr(Text::reload_do), ui::ButtonKind::primary}};
        content.default_button = 0;
        reload_dialog_.open(content, feedback);
    }

    // Every film as if "Refresh cover and info" had been chosen on its own page. The first one
    // that is refused (no key, no cover service) says why and the rest are not tried.
    void reload_everything(app::Feedback &feedback)
    {
        if (!svc_->refetch_cover)
        {
            notify(feedback, tr(Text::tmdb_no_key), audio::Cue::error);
            return;
        }
        const std::size_t waiting = svc_->cover_jobs; // films still fetched since the last scan
        std::size_t started = 0;
        for (std::size_t i = 0; i < library().size(); ++i)
        {
            const std::string refused = svc_->refetch_cover(movie(static_cast<int>(i)).id);
            if (!refused.empty())
            {
                notify(feedback, refused, audio::Cue::error);
                return;
            }
            ++started;
        }
        reload_total_ = started + waiting;
        reload_text_ = reload_progress_text(reload_total_);
        notify(feedback, reload_text_, audio::Cue::notify);
    }

    // "Fetching 3/26 ...": the job being worked on, counted from what the queue has left.
    std::string reload_progress_text(std::size_t left) const
    {
        const std::size_t finished = reload_total_ - std::min(left, reload_total_);
        const std::size_t current = std::min(finished + 1, reload_total_);
        return i18n::format(Text::reload_progress,
                            std::to_string(current) + "/" + std::to_string(reload_total_));
    }

    // The message stays up for as long as the reload runs. It gives way to any other message until
    // that has had its time, and to the film: it waits, finished or not, until the player closes.
    void update_reload_progress(app::Feedback &feedback)
    {
        if (reload_total_ == 0 || screen_ == Screen::player)
            return;
        if (svc_->cover_jobs == 0)
        {
            reload_total_ = 0;
            // A reload that was given up on has already said why (cover_note).
            if (svc_->cover_note.empty())
                notify(feedback, tr(Text::reload_finished), audio::Cue::notify);
            return;
        }
        const bool other_message = toast_time_ > 0.0f && toast_ != reload_text_;
        reload_text_ = reload_progress_text(svc_->cover_jobs);
        if (other_message)
            return;
        // Held above the point where draw_toast starts to fade; a new toast starts from the full
        // time, because that is what fades it in.
        toast_time_ = toast_time_ > 0.0f ? std::max(toast_time_, 1.5f) : 2.6f;
        toast_ = reload_text_;
    }

    const char *sort_name() const
    {
        switch (sort_)
        {
        case Sort::newest:
            return tr(Text::sort_newest);
        case Sort::year:
            return tr(Text::sort_year);
        default:
            return tr(Text::sort_title);
        }
    }

    void update_shelves(const InputFrame &input, app::Feedback &feedback)
    {
        Shelf &shelf = shelves_[static_cast<std::size_t>(row_)];
        const int count = static_cast<int>(shelf.movies.size());
        const int rows = static_cast<int>(shelves_.size());
        switch (input.nav)
        {
        case Direction::left:
        case Direction::right:
        {
            const int next = shelf.column + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < count)
            {
                shelf.column = next;
                feedback.play(audio::Cue::focus, 1.0f, ui::pan_for_x(card_rect(row_, next).cx()));
            }
            else
            {
                nudge_direction_ = input.nav == Direction::right ? 1.0f : -1.0f;
                refuse(feedback, input);
            }
            break;
        }
        case Direction::up:
        case Direction::down:
        {
            const int next = row_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < rows)
            {
                row_ = next;
                feedback.play(audio::Cue::tab);
            }
            else
            {
                nudge_direction_ = 0.0f;
                refuse(feedback, input);
            }
            break;
        }
        case Direction::none:
            break;
        }
    }

    void update_grid(const InputFrame &input, app::Feedback &feedback)
    {
        const int count = static_cast<int>(grid_.size());
        int next = grid_focus_;
        switch (input.nav)
        {
        case Direction::left:
            next = grid_focus_ % kGridColumns == 0 ? -1 : grid_focus_ - 1;
            break;
        case Direction::right:
            next = grid_focus_ % kGridColumns == kGridColumns - 1 || grid_focus_ + 1 >= count
                       ? -1
                       : grid_focus_ + 1;
            break;
        case Direction::up:
            next = grid_focus_ - kGridColumns < 0 ? -1 : grid_focus_ - kGridColumns;
            break;
        case Direction::down:
            next = grid_focus_ + kGridColumns >= count
                       ? (grid_focus_ / kGridColumns < (count - 1) / kGridColumns ? count - 1 : -1)
                       : grid_focus_ + kGridColumns;
            break;
        case Direction::none:
            return;
        }
        if (next < 0)
        {
            nudge_direction_ = input.nav == Direction::left    ? -1.0f
                               : input.nav == Direction::right ? 1.0f
                                                               : 0.0f;
            refuse(feedback, input);
            return;
        }
        grid_focus_ = next;
        feedback.play(audio::Cue::focus, 1.0f, ui::pan_for_x(grid_rect(next).cx()));
    }

    void open_details(app::Feedback &feedback)
    {
        screen_ = Screen::details;
        action_ = 0;
        action_position_.snap(0.0f);
        // The page shows what the file contains; reading it takes a moment on a
        // drive that has to spin up, so the page opens first and fills in.
        if (const int index = focused_movie(); index >= 0)
        {
            if (svc_->info != nullptr)
                svc_->info->request(movie(index).id, movie(index).path);
            sniff_subtitle_languages(movie(index));
        }
        feedback.play(audio::Cue::open);
    }

    // Moves to the neighbouring film of the shelf (or the grid) the page was opened from, so
    // Back lands on the film that was last shown. The ends refuse instead of wrapping.
    void step_details_movie(int step, const InputFrame &input, app::Feedback &feedback)
    {
        int *column =
            tab_ == Tab::home ? &shelves_[static_cast<std::size_t>(row_)].column : &grid_focus_;
        const int count =
            tab_ == Tab::home
                ? static_cast<int>(shelves_[static_cast<std::size_t>(row_)].movies.size())
                : static_cast<int>(grid_.size());
        const int next = *column + step;
        if (next < 0 || next >= count)
        {
            refuse(feedback, input);
            return;
        }
        *column = next;
        action_ = 0;
        action_position_.snap(0.0f);
        details_swap_.start(context_.settings.reduced_motion ? 0.1f : 0.32f);
        if (const int index = focused_movie(); index >= 0)
        {
            if (svc_->info != nullptr)
                svc_->info->request(movie(index).id, movie(index).path);
            sniff_subtitle_languages(movie(index));
        }
        feedback.play(audio::Cue::focus, step > 0 ? 1.1f : 0.9f, step > 0 ? 0.3f : -0.3f);
    }

    std::vector<SheetAction> sheet_actions() const
    {
        std::vector<SheetAction> actions = {SheetAction::play};
        const int index = focused_movie();
        if (index >= 0 && svc_->progress.in_progress(movie(index).id))
            actions.push_back(SheetAction::restart);
        actions.push_back(SheetAction::watched);
        if (svc_->refetch_cover)
            actions.push_back(SheetAction::cover);
        actions.push_back(SheetAction::close);
        return actions;
    }

    void update_details(const InputFrame &input, app::Feedback &feedback)
    {
        const std::vector<SheetAction> actions = sheet_actions();
        const int count = static_cast<int>(actions.size());
        action_ = std::min(action_, count - 1);
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next =
                std::clamp(action_ + (input.nav == Direction::down ? 1 : -1), 0, count - 1);
            if (next != action_)
            {
                action_ = next;
                feedback.play(audio::Cue::focus, 1.0f, 0.35f);
            }
        }
        int step = 0;
        if (input.nav == Direction::left || input.is_pressed(Action::page_prev))
            step = -1;
        else if (input.nav == Direction::right || input.is_pressed(Action::page_next))
            step = 1;
        if (step != 0)
        {
            step_details_movie(step, input, feedback);
            return;
        }
        const int focused = focused_movie();
        if (input.is_pressed(Action::confirm) && focused >= 0)
        {
            switch (actions[static_cast<std::size_t>(action_)])
            {
            case SheetAction::play:
                start_playback(focused, true, feedback);
                break;
            case SheetAction::restart:
                start_playback(focused, false, feedback);
                break;
            case SheetAction::watched:
                toggle_watched(focused, feedback);
                break;
            case SheetAction::cover:
            {
                const std::string refused = svc_->refetch_cover(movie(focused).id);
                if (refused.empty())
                    notify(feedback, tr(Text::fetching_info), audio::Cue::notify);
                else
                    notify(feedback, refused, audio::Cue::error);
                break;
            }
            case SheetAction::close:
                screen_ = Screen::browse;
                feedback.play(audio::Cue::modal_close);
                break;
            }
        }
        else if (input.is_pressed(Action::back))
        {
            screen_ = Screen::browse;
            feedback.play(audio::Cue::back);
        }
    }

    void toggle_watched(int index, app::Feedback &feedback)
    {
        const std::string &id = movie(index).id;
        const bool now_watched = !svc_->progress.watched(id);
        svc_->progress.set_watched(id, now_watched, svc_->now());
        feedback.play(now_watched ? audio::Cue::favorite_on : audio::Cue::favorite_off);
        toast_ = tr(now_watched ? Text::marked_watched : Text::marked_unwatched);
        toast_time_ = 2.2f;
        // A watched movie leaves Continue Watching.
        ++svc_->revision;
    }

    // ------------------------------------------------------------- player

    void start_playback(int index, bool resume, app::Feedback &feedback)
    {
        if (svc_->player == nullptr)
        {
            notify(feedback, tr(Text::playback_unavailable), audio::Cue::error);
            return;
        }
        const media::Movie &m = movie(index);
        const double start = resume ? svc_->progress.resume_position(m.id) : 0.0;
        svc_->player->open(m.path, start);
        playing_ = index;
        screen_ = Screen::player;
        osd_ = kOsdSeconds;
        paused_ = false;
        last_save_ = 0.0f;
        seek_flash_ = 0.0f;
        subtitle_track_ = -1;
        sniff_subtitle_languages(m);
        select_subtitle(m, 0);
        feedback.play(audio::Cue::launch);
        feedback.rumble(0.6f, 0.15f);
    }

    void save_progress()
    {
        if (svc_->player == nullptr || playing_ < 0)
            return;
        svc_->progress.update(movie(playing_).id, svc_->player->position(),
                              svc_->player->duration(), svc_->now());
    }

    void stop_playback(app::Feedback &feedback)
    {
        save_progress();
        if (svc_->player != nullptr)
            svc_->player->close();
        screen_ = Screen::browse;
        ++svc_->revision; // the shelves may need a Continue Watching entry
        feedback.play(audio::Cue::resume);
    }

    void update_player(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        media::Player &player = *svc_->player;
        player.update(dt);
        osd_ = std::max(0.0f, osd_ - dt);
        seek_flash_ = std::max(0.0f, seek_flash_ - dt);
        // A held direction arrives as repeats ~0.11 s apart; a longer gap means it was let go.
        seek_gap_ += dt;
        if (seek_gap_ > kSeekHoldGap)
            seek_hold_ = 0.0f;
        else
            seek_hold_ += dt;
        last_save_ += dt;
        const media::PlayerState state = player.state();

        if (state == media::PlayerState::playing && last_save_ >= kSaveEvery)
        {
            last_save_ = 0.0f;
            save_progress();
        }
        if (state == media::PlayerState::ended)
        {
            svc_->progress.set_watched(movie(playing_).id, true, svc_->now());
            player.close();
            screen_ = Screen::browse;
            ++svc_->revision;
            notify(feedback, tr(Text::finished), audio::Cue::complete);
            return;
        }
        const bool usable = state == media::PlayerState::playing ||
                            state == media::PlayerState::paused ||
                            state == media::PlayerState::buffering;

        if (input.is_pressed(Action::back))
        {
            stop_playback(feedback);
            return;
        }
        if (state == media::PlayerState::failed || !usable)
            return; // wait for the file to open; Circle (above) leaves

        const bool any_nav = input.nav != Direction::none || input.pressed != 0;
        if (any_nav)
            osd_ = kOsdSeconds;

        if (input.is_pressed(Action::confirm))
        {
            paused_ = !paused_;
            player.pause(paused_);
            feedback.play(paused_ ? audio::Cue::modal_open : audio::Cue::resume);
            osd_ = paused_ ? 3600.0f : kOsdSeconds; // a paused picture keeps its controls
        }
        double jump = 0.0;
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            // A tap jumps 10 s; holding adds 30 s per second held, up to 5 min.
            if (!input.nav_repeat || input.nav != seek_direction_)
                seek_hold_ = 0.0f;
            seek_direction_ = input.nav;
            seek_gap_ = 0.0f;
            const double step = input.nav_repeat ? held_seek_step(seek_hold_) : 10.0;
            jump = input.nav == Direction::right ? step : -step;
        }
        if (input.is_pressed(Action::jump_next) || input.is_pressed(Action::page_next))
            jump = 60.0;
        if (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::page_prev))
            jump = -60.0;
        if (jump != 0.0)
        {
            player.seek_by(jump);
            seek_flash_ = 0.9f;
            seek_amount_ = static_cast<float>(jump);
            feedback.play(audio::Cue::tick, jump > 0 ? 1.1f : 0.9f, jump > 0 ? 0.3f : -0.3f);
            if (paused_)
                osd_ = 3600.0f;
        }
        if (input.is_pressed(Action::west) && player.audio_tracks().size() > 1)
        {
            const int count = static_cast<int>(player.audio_tracks().size());
            const int next = (player.audio_track() + 1) % count;
            player.select_audio(next);
            notify(feedback,
                   i18n::format(Text::audio_track,
                                player.audio_tracks()[static_cast<std::size_t>(next)].label),
                   audio::Cue::toggle);
        }
        if (input.is_pressed(Action::north) && !movie(playing_).subtitles.empty())
            cycle_subtitles(feedback);
        if (input.is_pressed(Action::menu))
        {
            info_ = !info_;
            feedback.play(audio::Cue::tick);
        }
    }

    // Shows the film's subtitle file number `track`, or none for -1. A file that cannot be
    // read leaves the track selected but empty, so the pad still steps past it.
    // The language of a subtitle file: what its name says, and for a name that says nothing what
    // the words in the file say (looked at when the film's page opens, see below).
    std::string subtitle_language(const media::SubtitleFile &sub) const
    {
        if (sub.language != "und")
            return sub.language;
        const auto found = sniffed_subtitles_.find(sub.file);
        return found != sniffed_subtitles_.end() && !found->second.empty() ? found->second
                                                                           : sub.language;
    }

    // Reads each subtitle file of a film whose name does not give its language, once, and
    // guesses the language from its text. A file is a few hundred kilobytes at most, so the
    // page can wait for it.
    void sniff_subtitle_languages(const media::Movie &m)
    {
        for (const media::SubtitleFile &sub : m.subtitles)
        {
            if (sub.language != "und" || sniffed_subtitles_.count(sub.file) != 0)
                continue;
            std::string text;
            sniffed_subtitles_[sub.file] = save::read_file(sub.file, &text, 4u << 20)
                                               ? media::language_from_text(text)
                                               : std::string();
        }
    }

    void select_subtitle(const media::Movie &m, int track)
    {
        subtitle_track_ = track;
        subtitles_ = media::Subtitles{};
        if (track < 0 || static_cast<std::size_t>(track) >= m.subtitles.size())
            return;
        std::string text;
        if (save::read_file(m.subtitles[static_cast<std::size_t>(track)].file, &text, 4u << 20))
            subtitles_.parse(text);
    }

    // Triangle steps through the subtitle files and then off: with one file that is a plain
    // on/off switch.
    void cycle_subtitles(app::Feedback &feedback)
    {
        const media::Movie &m = movie(playing_);
        const int count = static_cast<int>(m.subtitles.size());
        const int next = subtitle_track_ + 1 < count ? subtitle_track_ + 1 : -1;
        select_subtitle(m, next);
        if (next < 0)
        {
            notify(feedback, tr(Text::subtitles_off), audio::Cue::toggle);
            return;
        }
        std::string label =
            media::language_names({subtitle_language(m.subtitles[static_cast<std::size_t>(next)])})
                .front();
        if (count > 1)
            label += " (" + std::to_string(next + 1) + "/" + std::to_string(count) + ")";
        notify(feedback, i18n::format(Text::subtitles_track, label), audio::Cue::toggle);
    }

    // ---------------------------------------------------------- animation

    void snap_layout()
    {
        hero_.snap(1.0f);
        row_position_.snap(0.0f);
        tone_.snap(tone_for(focused_movie()));
        ring_.snap(target_rect());
        sheet_.snap(0.0f);
    }

    Color tone_for(int index) const
    {
        if (index < 0)
            return Color::rgb(0x3b4a7a);
        const media::Movie &m = movie(index);
        if (svc_->posters != nullptr && !m.poster.empty())
            if (const media::Art *art = svc_->posters->get(m.poster, media::ArtKind::poster))
                return art->tone;
        return hsv(hash01(m.id, 1), 0.55f, 0.62f);
    }

    Rect target_rect() const
    {
        if (focused_movie() < 0)
            return {kMargin, kTopFull, kCardW, kCardH};
        return tab_ == Tab::home
                   ? grown(card_rect(row_, shelves_[static_cast<std::size_t>(row_)].column),
                           kCardGrow)
                   : grown(grid_rect(grid_focus_), 1.1f);
    }

    static Rect grown(const Rect &r, float factor)
    {
        const float w = r.w * factor;
        const float h = r.h * factor;
        return {r.cx() - w * 0.5f, r.cy() - h * 0.5f, w, h};
    }

    void animate(float dt)
    {
        const bool calm = context_.settings.reduced_motion;
        tab_age_ += dt;
        toast_time_ = std::max(0.0f, toast_time_ - dt);

        const int focused = focused_movie();
        if (focused >= 0 && static_cast<std::size_t>(focused) < library().size())
            focus_id_ = movie(focused).id;
        if (focused != shown_)
        {
            previous_ = shown_;
            shown_ = focused;
            hero_fade_.start(calm ? 0.1f : 0.42f);
        }
        hero_fade_.update(dt);
        details_swap_.update(dt);
        search_prompt_.update(dt);
        settings_list_.update(dt);
        settings_form_.update(dt);
        reload_dialog_.update(dt);

        // The billboard is open on the first shelf, or on every one when it is pinned.
        hero_.target =
            tab_ == Tab::home && (row_ == 0 || context_.settings.pin_billboard) ? 1.0f : 0.0f;
        hero_.update(dt, calm ? 30.0f : 9.0f);
        row_position_.target = static_cast<float>(row_);
        row_position_.update(dt, 11.0f);
        for (int r = 0; r < static_cast<int>(shelves_.size()); ++r)
        {
            Shelf &shelf = shelves_[static_cast<std::size_t>(r)];
            const float start = static_cast<float>(shelf.column) * (kCardW + kCardGap);
            shelf.scroll.reveal(start, start + kCardW * kCardGrow, gfx::kVirtualWidth - kMargin,
                                kMargin * 2.2f);
            shelf.scroll.update(dt, 12.0f);
        }
        if (tab_ == Tab::movies && !grid_.empty())
        {
            const float top = static_cast<float>(grid_focus_ / kGridColumns) * kGridPitchY;
            grid_scroll_.reveal(top, top + kGridH * 1.1f + 20.0f,
                                gfx::kVirtualHeight - kGridTop - 60.0f, 40.0f);
            grid_scroll_.update(dt, 12.0f);
        }
        tone_.target(tone_for(focused));
        tone_.update(dt, 4.0f);

        ring_.target(target_rect());
        ring_.update(dt, 20.0f);
        nudge_.update(dt, 9.0f);
        sheet_.target = screen_ == Screen::details ? 1.0f : 0.0f;
        sheet_.update(dt, calm ? 40.0f : 13.0f);
        action_position_.target = static_cast<float>(action_);
        action_position_.update(dt, 22.0f);
        osd_show_.target = osd_ > 0.0f ? 1.0f : 0.0f;
        osd_show_.update(dt, 12.0f);
    }

    // ------------------------------------------------------------ layout

    // A shelf card's rectangle at rest (the focused one grows around it).
    Rect card_rect(int row, int column) const
    {
        const Shelf &shelf = shelves_[static_cast<std::size_t>(row)];
        const float extra = kCardW * (kCardGrow - 1.0f);
        float x =
            kMargin + static_cast<float>(column) * (kCardW + kCardGap) - shelf.scroll.offset();
        if (row == row_)
        {
            if (column < shelf.column)
                x -= extra * 0.5f;
            else if (column > shelf.column)
                x += extra * 0.5f;
        }
        return {x, shelf_top(row), kCardW, kCardH};
    }

    float focus_top() const
    {
        return tween::lerp(kTopCompact, kTopFull, tween::clamp01(hero_.value));
    }

    float shelf_top(int row) const
    {
        return focus_top() + (static_cast<float>(row) - row_position_.value) * kRowPitch;
    }

    Rect grid_rect(int index) const
    {
        const int row = index / kGridColumns;
        const int column = index % kGridColumns;
        return {kMargin + static_cast<float>(column) * kGridPitchX,
                kGridTop + static_cast<float>(row) * kGridPitchY - grid_scroll_.offset(), kGridW,
                kGridH};
    }

    // --------------------------------------------------------- drawing

    // The art for one card, or a generated cover. alpha fades the whole card.
    void draw_poster(gfx::DrawList &list, int index, const Rect &rect, float radius,
                     float alpha) const
    {
        const media::Movie &m = movie(index);
        const media::Art *art = nullptr;
        if (svc_->posters != nullptr && !m.poster.empty())
            art = svc_->posters->get(m.poster, media::ArtKind::poster);
        // The generated cover stays underneath while the picture fades in.
        float fade = 0.0f;
        if (art != nullptr)
        {
            auto it = seen_.find(m.poster);
            if (it == seen_.end())
            {
                if (seen_.size() > 800)
                    seen_.clear();
                it = seen_.emplace(m.poster, clock_).first;
            }
            fade = tween::clamp01((clock_ - it->second) / 0.3f);
        }
        if (fade < 1.0f)
            draw_generated_cover(list, m, rect, radius, alpha);
        if (art != nullptr)
        {
            const Rect uv = cover_uv(static_cast<float>(art->width),
                                     static_cast<float>(art->height), rect.w / rect.h);
            list.image(art->texture, rect, uv, Color{1, 1, 1, alpha * fade}, radius);
        }
        if (const float done = svc_->progress.fraction(m.id); done > 0.0f)
        {
            const Rect bar{rect.x + rect.w * 0.08f, rect.y + rect.h - 16.0f, rect.w * 0.84f, 5.0f};
            list.rounded_rect(bar, 2.5f, Color::rgb(0x000000, 0.6f * alpha));
            list.rounded_rect({bar.x, bar.y, std::max(5.0f, bar.w * done), bar.h}, 2.5f,
                              kAccent.with_alpha(alpha));
        }
        else if (svc_->progress.watched(m.id))
        {
            const float r = rect.w * 0.075f;
            const float cx = rect.x + rect.w - r * 1.8f;
            const float cy = rect.y + r * 1.8f;
            list.circle(cx, cy, r, Color::rgb(0x000000, 0.7f * alpha));
            list.line(cx - r * 0.45f, cy + r * 0.05f, cx - r * 0.1f, cy + r * 0.4f, 2.4f,
                      kInk.with_alpha(alpha));
            list.line(cx - r * 0.1f, cy + r * 0.4f, cx + r * 0.5f, cy - r * 0.35f, 2.4f,
                      kInk.with_alpha(alpha));
        }
        if (m.quality == "4K")
        {
            const float w = rect.w * 0.2f;
            list.rounded_rect({rect.x + rect.w * 0.07f, rect.y + rect.w * 0.07f, w, rect.w * 0.12f},
                              4, kBlack.with_alpha(0.72f * alpha));
            ui::text(list, context_.fonts.semibold, "4K", rect.x + rect.w * 0.07f + w * 0.5f,
                     rect.y + rect.w * 0.07f + rect.w * 0.095f, rect.w * 0.085f,
                     kInk.with_alpha(alpha), gfx::Align::center);
        }
    }

    void draw_generated_cover(gfx::DrawList &list, const media::Movie &m, const Rect &rect,
                              float radius, float alpha) const
    {
        const float hue = hash01(m.id, 7);
        const Color top = hsv(hue, 0.52f, 0.58f).with_alpha(alpha);
        const Color bottom = hsv(hue + 0.06f, 0.62f, 0.2f).with_alpha(alpha);
        list.gradient_rect(rect, radius, top, bottom);
        list.circle(rect.x + rect.w * 0.82f, rect.y + rect.h * 0.18f, rect.w * 0.42f,
                    Color{1, 1, 1, 0.06f * alpha});
        list.circle(rect.x + rect.w * 0.1f, rect.y + rect.h * 0.9f, rect.w * 0.55f,
                    Color{0, 0, 0, 0.12f * alpha});
        const float size = rect.w * 0.125f;
        const float pad = rect.w * 0.1f;
        ui::paragraph(list, context_.fonts.display, m.title, rect.x + pad, rect.y + rect.h * 0.62f,
                      size, rect.w - 2.0f * pad, size * 1.22f, kInk.with_alpha(alpha), 4);
        if (m.year > 0)
        {
            char year[16];
            std::snprintf(year, sizeof(year), "%d", m.year);
            ui::text(list, context_.fonts.semibold, year, rect.x + pad,
                     rect.y + rect.h - pad * 0.9f, size * 0.82f, kInk.with_alpha(0.7f * alpha),
                     gfx::Align::left, 2.0f);
        }
    }

    void draw_page(app::Frame &frame) const
    {
        const Color tone = tone_.value();
        frame.backdrop.mode = gfx::BackdropMode::gradient;
        frame.backdrop.colors[0] = gfx::mix(tone, kBlack, 0.82f);
        frame.backdrop.colors[1] = kBlack;
        frame.backdrop.colors[2] = gfx::mix(tone, kBlack, 0.45f);
        frame.backdrop.params[0] = 0.78f;
        frame.backdrop.params[1] = 0.12f;
        frame.backdrop.params[2] = 0.55f;
        frame.backdrop.time = clock_;

        gfx::DrawList &list = frame.scene;
        const float back = tween::clamp01(sheet_.value);
        // Once the details cover the screen the page below is not drawn at all.
        if (back < 0.999f)
        {
            list.push_transform(1.0f + 0.04f * back, 960, 540, 0, 0);
            if (tab_ == Tab::settings)
            {
                draw_settings(list);
                draw_top_bar(list);
            }
            else if (library().size() == 0)
            {
                draw_top_bar(list);
                draw_empty(list);
            }
            else if (tab_ == Tab::home)
            {
                draw_hero(list);
                draw_shelves(list);
                draw_top_bar(list);
            }
            else
            {
                draw_grid(list);
                draw_top_bar(list);
            }
            list.pop_transform();
        }
        if (back > 0.01f && focused_movie() >= 0)
            draw_details(list);
        draw_toast(back > 0.5f ? frame.overlay : list);
        draw_hints(back > 0.5f ? frame.overlay : list);
        if (search_prompt_.visible())
        {
            ui::Canvas canvas{frame.overlay, context_.fonts, 0, clock_};
            search_prompt_.draw(canvas);
        }
        if (reload_dialog_.visible())
        {
            ui::Canvas canvas{frame.overlay, context_.fonts, 0, clock_};
            reload_dialog_.draw(canvas);
        }
    }

    void draw_settings(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(tab_age_, 0, 0.0f, 0.35f);
        list.push_opacity(in);
        ui::text(list, fonts.display, tr(Text::settings_title), kMargin, 214.0f, 52, kInk);
        ui::text(list, fonts.regular, tr(Text::settings_saved), kSettingsRight, 214.0f, 22,
                 kInk.with_alpha(0.5f), gfx::Align::right);
        ui::Canvas canvas{list, fonts, 0, clock_};
        settings_list_.draw(canvas);
        list.rounded_rect({(kMargin + kSettingsListWidth + kSettingsFormLeft) * 0.5f - 0.75f,
                           kSettingsTop, 1.5f, kSettingsHeight},
                          0.0f, kInk.with_alpha(0.12f));
        settings_form_.draw(canvas);
        list.pop_opacity();
    }

    // A gear drawn from shapes: eight teeth round a ring.
    static void draw_gear(gfx::DrawList &list, float cx, float cy, float radius, Color color)
    {
        constexpr int kTeeth = 8;
        constexpr float kTurn = 6.2831853f;
        for (int i = 0; i < kTeeth; ++i)
        {
            const float a = kTurn * static_cast<float>(i) / static_cast<float>(kTeeth);
            const float x = cx + std::sin(a) * radius * 0.86f;
            const float y = cy - std::cos(a) * radius * 0.86f;
            const float w = radius * 0.36f;
            const float h = radius * 0.42f;
            list.rotated_rect({x - w * 0.5f, y - h * 0.5f, w, h}, w * 0.2f, a, color);
        }
        list.ring(cx, cy, radius * 0.74f, radius * 0.3f, color);
    }

    void draw_top_bar(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        list.push_opacity(in);
        // A scrim keeps the bar legible over any billboard.
        list.gradient_rect({0, 0, gfx::kVirtualWidth, 170}, 0, kBlack.with_alpha(0.78f),
                           kBlack.with_alpha(0.0f));
        const float y = 86.0f - 8.0f * (1.0f - in);
        float x = ui::text(list, fonts.display, "MEDIATHEK", kMargin, y, 30, kAccent,
                           gfx::Align::left, 5.0f);
        // Version and author, small, under the logo.
        ui::text(list, fonts.regular, std::string("v") + kAppVersion + " \xC2\xB7 by m0", kMargin,
                 y + 30, 17, kInk.with_alpha(0.5f), gfx::Align::left, 1.0f);
        x = kMargin + x + 64.0f;
        const char *tabs[] = {tr(Text::tab_home), tr(Text::tab_movies)};
        for (int i = 0; i < 2; ++i)
        {
            const bool active = static_cast<int>(tab_) == i;
            const float w = ui::text(list, active ? fonts.semibold : fonts.regular, tabs[i], x,
                                     y - 2, 26, kInk.with_alpha(active ? 1.0f : 0.55f));
            if (active)
            {
                const float slide = tween::clamp01(tab_age_ / 0.3f);
                list.rounded_rect({x, y + 14, w * tween::cubic_out(slide), 4}, 2, kAccent);
            }
            x += w + 44.0f;
        }
        // The gear is the Settings tab, at the far right; the film count sits left of it.
        const bool gear_active = tab_ == Tab::settings;
        const float gear_x = gfx::kVirtualWidth - kMargin - kGearRadius;
        const float right = gear_x - kGearRadius - 36.0f;
        draw_gear(list, gear_x, y - 9.0f, kGearRadius, kInk.with_alpha(gear_active ? 1.0f : 0.55f));
        if (gear_active)
        {
            const float slide = tween::clamp01(tab_age_ / 0.3f);
            const float w = kGearRadius * 2.0f * tween::cubic_out(slide);
            list.rounded_rect({gear_x - kGearRadius, y + 14, w, 4}, 2, kAccent);
        }
        if (!svc_->library.movies().empty() || svc_->scanning)
        {
            ui::text(list, fonts.regular,
                     svc_->scanning && library().size() == 0 ? tr(Text::scanning)
                                                             : i18n::movie_count(library().size()),
                     right, y - 2, 22, kInk.with_alpha(0.6f), gfx::Align::right);
        }
        if (svc_->scanning)
        {
            const float cx = right - 150.0f - fonts.regular.measure(i18n::movie_count(9999), 22);
            list.arc(cx, y - 9, 11, 3, clock_ * 5.0f, 4.2f, kInk.with_alpha(0.8f));
        }
        list.pop_opacity();
    }

    static std::string fit(const ui::FontRef &font, const std::string &text, float size,
                           float width)
    {
        return font.font->fit(text, size, width);
    }

    // "English, Deutsch +2": as many names as fit in `width`, then how many more there are.
    static std::string list_fit(const ui::FontRef &font, const std::vector<std::string> &names,
                                float size, float width)
    {
        std::string text;
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            const std::string next = text.empty() ? names[i] : text + ", " + names[i];
            const std::size_t left = names.size() - i - 1;
            const std::string more = left > 0 ? "  +" + std::to_string(left) : std::string();
            if (i > 0 && font.measure(next + more, size) > width)
                return text + "  +" + std::to_string(names.size() - i);
            text = next;
        }
        return fit(font, text, size, width);
    }

    // One movie's billboard text, drawn at an opacity and a slide so two can cross-fade.
    void draw_hero_item(gfx::DrawList &list, int index, float alpha, float slide) const
    {
        if (alpha <= 0.01f || index < 0 || static_cast<std::size_t>(index) >= library().size())
            return;
        const ui::Fonts &fonts = context_.fonts;
        const media::Movie &m = movie(index);
        const float h = tween::clamp01(hero_.value);
        list.push_opacity(alpha);

        const float x = kMargin + slide;
        const Shelf &shelf = shelves_[static_cast<std::size_t>(row_)];
        const float title_size = tween::lerp(54.0f, 86.0f, h);
        const float title_y = tween::lerp(268.0f, 338.0f, h);
        // The red line names the shelf the film was found on.
        if (h > 0.02f)
            ui::text(list, fonts.semibold, ui::upper(shelf.title), x, title_y - 108.0f * h - 8.0f,
                     19, kAccent.with_alpha(h), gfx::Align::left, 4.0f);
        float size = title_size;
        const float width = fonts.display.measure(m.title, size);
        const float max_width = 1000.0f;
        if (width > max_width)
            size = std::max(title_size * 0.62f, size * max_width / width);
        ui::text(list, fonts.display, fit(fonts.display, m.title, size, max_width), x - 3, title_y,
                 size, kInk);
        const float meta_y = title_y + tween::lerp(40.0f, 52.0f, h);
        ui::text(list, fonts.regular, meta_line(m), x, meta_y, tween::lerp(23.0f, 26.0f, h),
                 kInk.with_alpha(0.78f));

        if (h > 0.02f)
        {
            list.push_opacity(h);
            const float bar_y = meta_y + 34.0f;
            const float done = svc_->progress.fraction(m.id);
            if (done > 0.0f)
            {
                const Rect bar{x, bar_y, 360, 6};
                list.rounded_rect(bar, 3, kInk.with_alpha(0.2f));
                list.rounded_rect({bar.x, bar.y, std::max(6.0f, bar.w * done), bar.h}, 3, kAccent);
                const std::string text =
                    i18n::format(Text::time_left, format_time(svc_->progress.find(m.id)->duration -
                                                              svc_->progress.find(m.id)->position));
                ui::text(list, fonts.regular, text, bar.x + bar.w + 20, bar.y + 10, 21,
                         kInk.with_alpha(0.7f));
            }
            const float by = bar_y + (done > 0.0f ? 36.0f : 6.0f);
            const bool resuming = svc_->progress.in_progress(m.id);
            const Rect play{x, by, resuming ? 250.0f : 214.0f, 64};
            list.glow(play, 32, 20, kAccent.with_alpha(0.32f));
            list.rounded_rect(play, 32, kInk);
            list.triangle({play.x + 30, play.cy() - 13, 24, 26}, kBlack, 0.0f, 1.5708f);
            ui::text(list, fonts.semibold, tr(resuming ? Text::button_resume : Text::button_play),
                     play.x + 74, play.cy() + 10, 28, kBlack);
            list.pop_opacity();
        }
        list.pop_opacity();
    }

    void draw_backdrop_art(gfx::DrawList &list, int index, float alpha) const
    {
        if (alpha <= 0.01f || index < 0 || static_cast<std::size_t>(index) >= library().size())
            return;
        const media::Movie &m = movie(index);
        const float h = tween::clamp01(hero_.value);
        const float height = tween::lerp(560.0f, 900.0f, h);
        const Rect area{0, 0, gfx::kVirtualWidth, height};
        const Color tone = tone_.value();

        const media::Art *wide = nullptr;
        if (svc_->posters != nullptr && !m.backdrop.empty())
            wide = svc_->posters->get(m.backdrop, media::ArtKind::backdrop);
        if (wide != nullptr)
        {
            const Rect uv = cover_uv(static_cast<float>(wide->width),
                                     static_cast<float>(wide->height), area.w / area.h);
            const float strength = tween::lerp(0.3f, 0.92f, h) * alpha;
            list.image_gradient(wide->texture, area, uv, Color{1, 1, 1, strength},
                                Color{1, 1, 1, 0.0f * strength});
        }
        else
        {
            // No fanart: the poster itself, large, on the right.
            const media::Art *poster = nullptr;
            if (svc_->posters != nullptr && !m.poster.empty())
                poster = svc_->posters->get(m.poster, media::ArtKind::poster);
            const float bob =
                context_.settings.reduced_motion ? 0.0f : std::sin(clock_ * 0.8f) * 5.0f;
            const Rect art{1290.0f, 120.0f + bob - 40.0f * (1.0f - h), 400.0f, 600.0f};
            list.push_opacity(alpha * h);
            list.glow(art.inset(40), 40, 100, tone.with_alpha(0.28f));
            list.shadow({art.x, art.y + 30, art.w, art.h}, 28, 50, Color::rgb(0x000000, 0.6f));
            if (poster != nullptr)
                list.image(poster->texture, art,
                           cover_uv(static_cast<float>(poster->width),
                                    static_cast<float>(poster->height), art.w / art.h),
                           kInk, 24);
            else
                draw_generated_cover(list, m, art, 24, 1.0f);
            list.bordered_rect(art, 24, Color{0, 0, 0, 0}, 2, kInk.with_alpha(0.14f));
            list.pop_opacity();
        }
        // Scrims. Nothing may end in a hard edge: the page behind is tinted, so
        // a black block that stops at `height` shows up as a seam across the
        // shelves. The text-side scrim is therefore stepped down to nothing at
        // the bottom, and the vertical one is a bump that returns to zero.
        constexpr float kFade = 280.0f;
        constexpr int kSteps = 28;
        const float side = 0.86f * alpha;
        list.gradient_rect_h({0, 0, 1280, std::round(height - kFade)}, 0, kBlack.with_alpha(side),
                             kBlack.with_alpha(0.0f));
        // Whole-pixel strips that touch but do not overlap: overlaps double
        // the alpha and fractional edges leave hairlines.
        for (int i = 0; i < kSteps; ++i)
        {
            const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(kSteps);
            const float y0 = std::round(height - kFade +
                                        kFade * static_cast<float>(i) / static_cast<float>(kSteps));
            const float y1 = std::round(
                height - kFade + kFade * static_cast<float>(i + 1) / static_cast<float>(kSteps));
            list.gradient_rect_h({0, y0, 1280, y1 - y0}, 0, kBlack.with_alpha(side * (1.0f - t)),
                                 kBlack.with_alpha(0.0f));
        }
        const float peak = tween::lerp(0.35f, 0.85f, h) * alpha;
        list.gradient_rect({0, height - 440, gfx::kVirtualWidth, 260}, 0, kBlack.with_alpha(0.0f),
                           kBlack.with_alpha(peak));
        list.gradient_rect({0, height - 180, gfx::kVirtualWidth, 180}, 0, kBlack.with_alpha(peak),
                           kBlack.with_alpha(0.0f));
    }

    void draw_hero(gfx::DrawList &list) const
    {
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        if (hero_fade_.running)
        {
            const float t = hero_fade_.progress();
            draw_backdrop_art(list, previous_, 1.0f - tween::smoothstep(t));
            draw_backdrop_art(list, shown_, tween::smoothstep(t));
            draw_hero_item(list, previous_, 1.0f - tween::smoothstep(t * 2.2f),
                           -36.0f * tween::cubic_in(tween::clamp01(t * 2.2f)));
            const float arrive = tween::clamp01((t - 0.25f) / 0.75f);
            draw_hero_item(list, shown_, tween::smoothstep(arrive),
                           44.0f * (1.0f - tween::quint_out(arrive)));
        }
        else
        {
            draw_backdrop_art(list, shown_, 1.0f);
            draw_hero_item(list, shown_, 1.0f, 30.0f * (1.0f - in));
        }
        list.pop_opacity();
    }

    void draw_shelves(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float nudge = ui::shake(nudge_.value, clock_, 16.0f, 8.0f) * nudge_direction_;
        for (int r = 0; r < static_cast<int>(shelves_.size()); ++r)
        {
            const Shelf &shelf = shelves_[static_cast<std::size_t>(r)];
            const float distance = static_cast<float>(r) - row_position_.value;
            // Shelves above the focused one are gone; the one below peeks in.
            const float visible =
                tween::clamp01(1.0f + distance * 2.5f) *
                (distance > 0.0f ? 1.0f - 0.35f * tween::clamp01(distance) : 1.0f);
            if (visible <= 0.01f)
                continue;
            const float in = tween::stagger(age_, 3 + r, 0.09f, 0.6f);
            list.push_opacity(visible * in);
            const float y = shelf_top(r) + 40.0f * (1.0f - in);
            ui::text(list, fonts.semibold, shelf.title, kMargin, y - 46, 25,
                     kInk.with_alpha(r == row_ ? 0.96f : 0.62f));
            if (r == row_)
            {
                char count[24];
                std::snprintf(count, sizeof(count), "%d / %zu", shelf.column + 1,
                              shelf.movies.size());
                ui::text(list, fonts.mono, count, gfx::kVirtualWidth - kMargin, y - 46, 20,
                         kInk.with_alpha(0.45f), gfx::Align::right);
            }
            for (int c = 0; c < static_cast<int>(shelf.movies.size()); ++c)
            {
                Rect rect = card_rect(r, c);
                rect.y = y;
                if (rect.x > gfx::kVirtualWidth || rect.x + rect.w < -80.0f)
                    continue;
                if (r == row_ && c == shelf.column)
                    continue; // drawn last, on top
                draw_poster(list, shelf.movies[static_cast<std::size_t>(c)], rect, 14,
                            r == row_ ? 0.94f : 0.7f);
            }
            list.pop_opacity();
        }

        // The focused card: grown, lifted, ringed.
        if (focused_movie() < 0)
            return;
        Rect ring = ring_.value();
        ring.x += nudge;
        const float in = tween::stagger(age_, 3 + row_, 0.09f, 0.6f);
        list.push_opacity(in);
        draw_focused_card(list, focused_movie(), ring);
        list.pop_opacity();
    }

    void draw_focused_card(gfx::DrawList &list, int index, const Rect &ring) const
    {
        const Color tone = tone_.value();
        list.shadow({ring.x, ring.y + 20, ring.w, ring.h}, 18, 36, Color::rgb(0x000000, 0.65f));
        list.glow(ring, 18, 26, tone.with_alpha(0.38f + 0.14f * ui::breathe(clock_)));
        draw_poster(list, index, ring, 14, 1.0f);
        list.bordered_rect(ring.inset(-4), 17, Color{0, 0, 0, 0}, 3.5f, kInk);
    }

    void draw_grid(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(tab_age_, 0, 0.0f, 0.35f);
        list.push_opacity(in);
        // Header: what this page is, and how it is sorted.
        ui::text(list, fonts.display, tr(Text::shelf_all), kMargin, 168, 34, kInk);
        std::string state = i18n::format(Text::sorted_by, sort_name());
        if (!search_.empty())
        {
            // The search, in the accent colour, where the eye already is.
            const float x = kMargin + fonts.display.measure(tr(Text::shelf_all), 34) + 36.0f;
            const std::string count =
                grid_.size() == 1 ? std::string(tr(Text::movies_one))
                                  : i18n::format(Text::movies_many, std::to_string(grid_.size()));
            ui::text(list, fonts.semibold,
                     fit(fonts.semibold,
                         i18n::format(Text::search_active, search_) + "  \xC2\xB7  " + count, 24,
                         1000.0f),
                     x, 166, 24, kAccent);
        }
        ui::text(list, fonts.regular, state, gfx::kVirtualWidth - kMargin, 164, 22,
                 kInk.with_alpha(0.6f), gfx::Align::right);
        if (grid_.empty())
        {
            const float cx = gfx::kVirtualWidth * 0.5f;
            ui::text(list, fonts.display, tr(Text::search_none), cx, 480, 44, kInk,
                     gfx::Align::center);
            ui::text(list, fonts.regular, tr(Text::search_none_hint), cx, 540, 26,
                     kInk.with_alpha(0.65f), gfx::Align::center);
        }

        list.push_clip({0, kGridFadeTop, gfx::kVirtualWidth, gfx::kVirtualHeight - kGridFadeTop});
        const float nudge = ui::shake(nudge_.value, clock_, 14.0f, 8.0f) * nudge_direction_;
        for (int i = 0; i < static_cast<int>(grid_.size()); ++i)
        {
            Rect rect = grid_rect(i);
            if (rect.y > gfx::kVirtualHeight || rect.y + rect.h < kGridTop - 40.0f)
                continue;
            if (i == grid_focus_)
                continue;
            const float appear =
                tween::stagger(tab_age_, (i / kGridColumns) + (i % kGridColumns) / 3, 0.03f, 0.4f);
            rect.y += 24.0f * (1.0f - appear);
            // A card leaving upwards fades as it passes the header, not cut off by it.
            const float leaving = tween::smoothstep(
                tween::clamp01((rect.y + rect.h * 0.95f - kGridFadeTop) / 120.0f));
            list.push_opacity(appear * leaving);
            draw_poster(list, grid_[static_cast<std::size_t>(i)], rect, 12, 0.88f);
            list.pop_opacity();
        }
        if (!grid_.empty())
        {
            Rect ring = ring_.value();
            ring.x += nudge;
            draw_focused_card(list, grid_[static_cast<std::size_t>(grid_focus_)], ring);
        }
        list.pop_clip();
        list.pop_opacity();

        // The focused movie, bottom left.
        if (const int focused = focused_movie(); focused >= 0)
        {
            const media::Movie &m = movie(focused);
            // Opaque under the hint row, or the covers read through its text.
            list.gradient_rect({0, 850, gfx::kVirtualWidth, 130}, 0, kBlack.with_alpha(0.0f),
                               kBlack.with_alpha(0.97f));
            list.rounded_rect({0, 980, gfx::kVirtualWidth, 100}, 0, kBlack.with_alpha(0.97f));
            ui::text(list, fonts.semibold, fit(fonts.semibold, m.title, 30, 1000), kMargin, 1000,
                     30, kInk);
            ui::text(list, fonts.regular, meta_line(m), kMargin, 1036, 22, kInk.with_alpha(0.65f));
        }
    }

    void draw_empty(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 1, 0.08f, 0.7f);
        list.push_opacity(in);
        const float cx = gfx::kVirtualWidth * 0.5f;
        const float cy = 470.0f - 20.0f * (1.0f - in);
        // A film reel drawn from shapes.
        list.circle(cx, cy - 120, 78, kInk.with_alpha(0.08f));
        list.ring(cx, cy - 120, 78, 5, kInk.with_alpha(0.5f));
        for (int i = 0; i < 6; ++i)
        {
            const float a = clock_ * 0.4f + static_cast<float>(i) * 1.0471976f;
            list.circle(cx + std::sin(a) * 44, cy - 120 - std::cos(a) * 44, 13,
                        kInk.with_alpha(0.55f));
        }
        list.circle(cx, cy - 120, 9, kInk.with_alpha(0.7f));
        ui::text(list, fonts.display,
                 tr(svc_->scanning ? Text::looking_for_movies : Text::no_movies_found), cx, cy + 30,
                 52, kInk, gfx::Align::center);
        ui::text(list, fonts.regular, tr(Text::put_files_in), cx, cy + 90, 26,
                 kInk.with_alpha(0.7f), gfx::Align::center);
        const char *places[] = {tr(Text::place_internal), tr(Text::place_usb),
                                tr(Text::place_extended)};
        for (int i = 0; i < 3; ++i)
            ui::text(list, fonts.mono, places[i], cx, cy + 148 + static_cast<float>(i) * 40, 24,
                     kInk.with_alpha(0.9f), gfx::Align::center);
        ui::text(list, fonts.regular, tr(Text::name_them), cx, cy + 300, 22, kInk.with_alpha(0.55f),
                 gfx::Align::center);
        if (!svc_->scan_note.empty())
            ui::text(list, fonts.regular, svc_->scan_note, cx, cy + 346, 22, kAccent,
                     gfx::Align::center);
        list.pop_opacity();
    }

    // The wallpaper of the page behind the details, over the whole screen. A
    // film without one (or with the setting off) leaves the tinted page colour, with a glow.
    void draw_details_art(gfx::DrawList &list, int index, float t) const
    {
        const media::Movie &m = movie(index);
        const Color tone = tone_.value();
        const media::Art *wide = nullptr;
        if (svc_->posters != nullptr && !m.backdrop.empty() && context_.settings.details_wallpaper)
            wide = svc_->posters->get(m.backdrop, media::ArtKind::backdrop);
        // It drifts out to its full size as the page opens, like the hero.
        const float grow = 28.0f * (1.0f - t);
        const Rect area{-grow, -grow * 0.5625f, gfx::kVirtualWidth + 2.0f * grow,
                        gfx::kVirtualHeight + 1.125f * grow};
        if (wide != nullptr)
        {
            const Rect uv = cover_uv(static_cast<float>(wide->width),
                                     static_cast<float>(wide->height), area.w / area.h);
            list.image_gradient(wide->texture, area, uv, Color{1, 1, 1, 0.92f * t},
                                Color{1, 1, 1, 0.5f * t});
        }
        else
        {
            list.glow({1100, 120, 800, 800}, 400, 220, tone.with_alpha(0.3f * t));
        }
        // The text side stays dark whatever the picture is; the bottom keeps the
        // button hints readable.
        list.gradient_rect_h({0, 0, 1500, gfx::kVirtualHeight}, 0, kBlack.with_alpha(0.86f * t),
                             kBlack.with_alpha(0.0f));
        list.gradient_rect({0, 560, gfx::kVirtualWidth, 520}, 0, kBlack.with_alpha(0.0f),
                           kBlack.with_alpha(0.78f * t));
    }

    // The full-screen page of one movie: the cover on the left, the facts and
    // the description in the middle, the actions on the right.
    void draw_details(gfx::DrawList &list) const
    {
        const int index = focused_movie();
        if (index < 0)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const media::Movie &m = movie(index);
        const float t = tween::clamp01(sheet_.value);
        const Color tone = tone_.value();

        // A veil of the page colour hides the page behind it: fading the page
        // itself would turn its shadows grey.
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                          gfx::mix(tone, kBlack, 0.82f).with_alpha(t));
        // A film change fades the new page in over the veil; opening the page does not.
        const float swap = details_swap_.running
                               ? 0.25f + 0.75f * tween::cubic_out(details_swap_.progress())
                               : 1.0f;
        draw_details_art(list, index, t * swap);
        list.push_opacity(swap);

        constexpr float kCenter = gfx::kVirtualHeight * 0.5f;
        constexpr float kColumn = 600.0f;
        constexpr float kColumnWidth = 760.0f;

        // The cover, vertically centred, sliding in from the left.
        const float arrive = tween::stagger(t, 0, 0.12f, 0.6f);
        const Rect art{120.0f - 40.0f * (1.0f - arrive), kCenter - 300.0f, 400, 600};
        list.push_opacity(arrive);
        list.glow(art.inset(40), 40, 100, tone.with_alpha(0.3f));
        list.shadow({art.x, art.y + 30, art.w, art.h}, 28, 50, Color::rgb(0x000000, 0.6f));
        draw_poster(list, index, art, 24, 1.0f);
        list.bordered_rect(art, 24, Color{0, 0, 0, 0}, 2, kInk.with_alpha(0.14f));
        list.pop_opacity();

        // Title and the facts the Home billboard shows.
        const float head = tween::stagger(t, 1, 0.12f, 0.6f);
        const float top = art.y;
        list.push_opacity(head);
        // What the file says about itself, once it has been read.
        const media::InfoService::Lookup lookup =
            svc_->info != nullptr ? svc_->info->peek(m.id) : media::InfoService::Lookup{};
        const media::MediaInfo *content = lookup.info.get();

        // Where the film lives, and what kind of film it is.
        std::string label = m.collection.empty() ? m.drive : m.collection;
        for (const int genre : media::main_genres(m))
            label += "  \xC2\xB7  " + media::genre_name(genre);
        ui::text(list, fonts.semibold,
                 fit(fonts.semibold, ui::upper(label), 19, kColumnWidth - 120), kColumn, top + 36,
                 19, kAccent, gfx::Align::left, 4.0f);
        float size = 78.0f;
        const float width = fonts.display.measure(m.title, size);
        if (width > kColumnWidth)
            size = std::max(50.0f, size * kColumnWidth / width);
        ui::text(list, fonts.display, fit(fonts.display, m.title, size, kColumnWidth), kColumn - 3,
                 top + 36 + 14 + size * 0.86f, size, kInk);
        const float meta_y = top + 36 + 14 + size * 0.86f + 52.0f;
        {
            // The facts as chips: year, TMDB's user score, length, picture, sound. The last
            // ones are dropped where they would run into the buttons.
            struct Chip
            {
                std::string text;
                bool star = false;
            };
            std::vector<Chip> chips;
            if (m.year > 0)
                chips.push_back({std::to_string(m.year), false});
            if (m.rating > 0.0f)
            {
                char score[16];
                std::snprintf(score, sizeof(score), "%.1f", static_cast<double>(m.rating));
                chips.push_back({score, true});
            }
            if (content != nullptr && !media::format_runtime(content->duration).empty())
                chips.push_back({media::format_runtime(content->duration), false});
            std::string picture;
            const auto join = [&picture](const std::string &part)
            {
                if (part.empty())
                    return;
                picture += picture.empty() ? "" : "  \xC2\xB7  ";
                picture += part;
            };
            if (content != nullptr)
            {
                const std::string size_label =
                    media::resolution_label(content->width, content->height);
                join(content->video_codec);
                join(size_label.empty() ? m.quality : size_label);
                join(content->container);
            }
            else
            {
                join(m.quality);
            }
            if (!picture.empty())
                chips.push_back({picture, false});
            if (content != nullptr && !content->audio_format.empty())
                chips.push_back({content->audio_format, false});

            const float chip_y = meta_y - 29.0f;
            float chip_x = kColumn;
            for (const Chip &chip : chips)
            {
                const float width =
                    fonts.semibold.measure(chip.text, 22) + 36.0f + (chip.star ? 26.0f : 0.0f);
                if (chip_x + width > kColumn + kColumnWidth + 60.0f)
                    break;
                list.bordered_rect({chip_x, chip_y, width, 40.0f}, 20, kInk.with_alpha(0.08f), 1.5f,
                                   kInk.with_alpha(0.22f));
                float text_x = chip_x + 18.0f;
                if (chip.star)
                {
                    list.star(text_x + 9.0f, chip_y + 20.0f, 9.0f, Color::rgb(0xffc94a));
                    text_x += 26.0f;
                }
                ui::text(list, fonts.semibold, chip.text, text_x, chip_y + 27.0f, 22,
                         kInk.with_alpha(0.92f));
                chip_x += width + 10.0f;
            }
        }
        const double resume = svc_->progress.resume_position(m.id);
        float y = meta_y + 30.0f;
        const float done = svc_->progress.fraction(m.id);
        if (done > 0.0f)
        {
            const Rect bar{kColumn, y, 360, 6};
            list.rounded_rect(bar, 3, kInk.with_alpha(0.2f));
            list.rounded_rect({bar.x, bar.y, std::max(6.0f, bar.w * done), bar.h}, 3, kAccent);
            const media::Watch *entry = svc_->progress.find(m.id);
            if (entry != nullptr)
                ui::text(
                    list, fonts.regular,
                    i18n::format(Text::time_left, format_time(entry->duration - entry->position)),
                    bar.x + bar.w + 20, bar.y + 10, 21, kInk.with_alpha(0.7f));
            y += 30.0f;
        }
        list.pop_opacity();

        // The description, from TMDB; a film it does not know has none.
        const float body = tween::stagger(t, 2, 0.12f, 0.6f);
        list.push_opacity(body);
        constexpr float kLine = 36.0f;
        const float text_y = y + 40.0f + 8.0f * (1.0f - body);
        // The file and status sit at the foot of the cover; a long description stops
        // (with an ellipsis) a line above their labels instead of running into them.
        const float facts_y = art.y + art.h - 4.0f;
        const int fits = static_cast<int>((facts_y - 100.0f - 40.0f - text_y) / kLine) + 1;
        if (!m.overview.empty())
            ui::paragraph(list, fonts.regular, m.overview, kColumn, text_y, 24, kColumnWidth, kLine,
                          kInk.with_alpha(0.86f), std::clamp(fits, 1, 9));
        else
            ui::paragraph(list, fonts.regular,
                          tr(svc_->refetch_cover ? Text::no_description_try : Text::no_description),
                          kColumn, text_y, 24, kColumnWidth, kLine, kInk.with_alpha(0.5f), 3);
        list.pop_opacity();

        // The file and where the viewer stands, small, under the description.
        const float facts = tween::stagger(t, 3, 0.12f, 0.6f);
        list.push_opacity(facts);
        const std::string file =
            m.path.substr(m.path.rfind('/') == std::string::npos ? 0 : m.path.rfind('/') + 1);
        const std::string status = svc_->progress.watched(m.id) ? tr(Text::status_watched)
                                   : resume > 0.0
                                       ? i18n::format(Text::status_at, format_time(resume))
                                       : tr(Text::status_not_started);
        constexpr float kSecond = kColumn + 440.0f; // the second column of the foot rows
        const auto caption = [&](const std::string &text, float x, float y)
        {
            ui::text(list, fonts.semibold, text, x, y, 15, kInk.with_alpha(0.5f), gfx::Align::left,
                     3.0f);
        };
        // Which languages the film offers: the sound the player can play (read from the
        // file, so it may still be on its way) and the .srt beside the film.
        const auto languages =
            [&](const std::vector<std::string> &names, float x, float width, bool from_file)
        {
            const bool known = !names.empty();
            const std::string text = known ? list_fit(fonts.regular, names, 22, width)
                                     : content != nullptr || !from_file ? tr(Text::info_none)
                                     : lookup.state == media::InfoService::State::pending
                                         ? tr(Text::info_reading)
                                         : tr(Text::info_unavailable);
            ui::text(list, fonts.regular, text, x, facts_y - 70, 22,
                     kInk.with_alpha(known ? 0.92f : 0.5f));
        };
        caption(tr(Text::label_audio), kColumn, facts_y - 100);
        // The file's own tags first; a film whose file says nothing (or could not be read) is
        // as its name says, "German" in "Film (2011) German DL.mkv".
        const bool probed = content != nullptr || lookup.state == media::InfoService::State::failed;
        languages(media::language_names(media::audio_languages_to_show(content, probed, file)),
                  kColumn, 400.0f, true);
        caption(tr(Text::label_subtitles), kSecond, facts_y - 100);
        std::vector<std::string> subtitle_languages;
        for (const media::SubtitleFile &sub : m.subtitles)
            media::add_language(&subtitle_languages, subtitle_language(sub));
        languages(media::language_names(subtitle_languages), kSecond, 1360.0f - kSecond, false);
        caption(m.size > 0
                    ? std::string(tr(Text::label_file)) + "  \xC2\xB7  " + format_size(m.size)
                    : std::string(tr(Text::label_file)),
                kColumn, facts_y - 30);
        ui::text(list, fonts.mono, fit(fonts.mono, file, 20, 400), kColumn, facts_y, 20,
                 kInk.with_alpha(0.9f));
        caption(tr(Text::label_status), kSecond, facts_y - 30);
        ui::text(list, fonts.semibold, status, kSecond, facts_y, 24, kInk);
        list.pop_opacity();

        // Actions: one highlight that springs between the rows.
        const std::vector<SheetAction> actions = sheet_actions();
        const float ax = 1824.0f - 384.0f + 36.0f * (1.0f - tween::stagger(t, 2, 0.12f, 0.6f));
        const float ay = kCenter - (static_cast<float>(actions.size()) * 84.0f - 12.0f) * 0.5f;
        list.push_opacity(tween::stagger(t, 2, 0.12f, 0.6f));
        list.rounded_rect({ax, ay + action_position_.value * 84, 384, 72}, 36, kInk);
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            const bool focused = static_cast<int>(i) == action_;
            const float row = ay + static_cast<float>(i) * 84;
            std::string text;
            switch (actions[i])
            {
            case SheetAction::play:
                text = resume > 0.0 ? i18n::format(Text::resume_from, format_time(resume))
                                    : tr(Text::button_play);
                break;
            case SheetAction::restart:
                text = tr(Text::start_over);
                break;
            case SheetAction::watched:
                text = tr(svc_->progress.watched(m.id) ? Text::mark_unwatched : Text::mark_watched);
                break;
            case SheetAction::cover:
                text = tr(Text::refresh_info);
                break;
            case SheetAction::close:
                text = tr(Text::action_back);
                break;
            }
            if (!focused)
                list.bordered_rect({ax, row, 384, 72}, 36, kInk.with_alpha(0.08f), 1.5f,
                                   kInk.with_alpha(0.22f));
            ui::text(list, fonts.semibold, text, ax + 36, row + 46, 25,
                     focused ? kBlack : kInk.with_alpha(0.9f));
        }
        list.pop_opacity();
        list.pop_opacity(); // the film change
    }

    void draw_toast(gfx::DrawList &list) const
    {
        if (toast_time_ <= 0.0f || toast_.empty())
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float a =
            tween::clamp01(std::min(toast_time_ * 4.0f, (2.6f - toast_time_) * 6.0f + 0.001f));
        const float w = fonts.semibold.measure(toast_, 24) + 64;
        const Rect pill{gfx::kVirtualWidth * 0.5f - w * 0.5f, 906.0f - 14.0f * (1.0f - a), w, 56};
        list.push_opacity(a);
        list.shadow({pill.x, pill.y + 8, pill.w, pill.h}, 28, 24, Color::rgb(0x000000, 0.5f));
        list.rounded_rect(pill, 28, Color::rgb(0x15161f, 0.96f));
        list.bordered_rect(pill, 28, Color{0, 0, 0, 0}, 1.5f, kInk.with_alpha(0.2f));
        ui::text(list, fonts.semibold, toast_, pill.cx(), pill.y + 37, 24, kInk,
                 gfx::Align::center);
        list.pop_opacity();
    }

    void draw_hints(gfx::DrawList &list) const
    {
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        if (screen_ == Screen::details)
        {
            const ui::Hint hints[] = {{ui::Button::cross, tr(Text::hint_choose)},
                                      {ui::Button::l1, tr(Text::hint_movie), ui::Button::r1},
                                      {ui::Button::circle, tr(Text::hint_back)}};
            ui::draw_hints(list, context_.fonts, style, hints, 3, 1824, true);
        }
        else if (tab_ == Tab::settings)
        {
            const ui::Hint hints[] = {{ui::Button::cross, settings_in_form_ ? tr(Text::hint_change)
                                                                            : tr(Text::hint_open)},
                                      {ui::Button::circle, tr(Text::hint_back)},
                                      {ui::Button::l1, tr(Text::hint_tab), ui::Button::r1}};
            ui::draw_hints(list, context_.fonts, style, hints, 3, 1824, true);
        }
        else if (library().size() == 0)
        {
            const ui::Hint hints[] = {{ui::Button::options, tr(Text::hint_look_again)}};
            ui::draw_hints(list, context_.fonts, style, hints, 1, 1824, true);
        }
        else if (tab_ == Tab::home)
        {
            const ui::Hint hints[] = {{ui::Button::cross, tr(Text::hint_details)},
                                      {ui::Button::square, tr(Text::hint_play)},
                                      {ui::Button::triangle, tr(Text::hint_watched)},
                                      {ui::Button::l1, tr(Text::hint_tab), ui::Button::r1}};
            ui::draw_hints(list, context_.fonts, style, hints, 4, 1824, true);
        }
        else
        {
            const ui::Hint hints[] = {{ui::Button::cross, tr(Text::hint_details)},
                                      {ui::Button::square, tr(Text::hint_play)},
                                      {ui::Button::triangle, tr(Text::hint_search)},
                                      {ui::Button::right_stick, tr(Text::hint_sort)},
                                      {ui::Button::l1, tr(Text::hint_tab), ui::Button::r1}};
            ui::draw_hints(list, context_.fonts, style, hints, 5, 1824, true);
        }
    }

    // ---------------------------------------------------------- the stage

    void draw_player(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const media::Player *player = svc_->player;
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x000000));
        if (player == nullptr || playing_ < 0)
            return;
        const media::Movie &m = movie(playing_);
        const media::PlayerState state = player->state();

        if (player->texture() != 0 && player->video_width() > 0)
        {
            const float aspect = static_cast<float>(player->video_width()) /
                                 static_cast<float>(player->video_height());
            float w = gfx::kVirtualWidth;
            float h = w / aspect;
            if (h > gfx::kVirtualHeight)
            {
                h = gfx::kVirtualHeight;
                w = h * aspect;
            }
            list.image(player->texture(),
                       {(gfx::kVirtualWidth - w) * 0.5f, (gfx::kVirtualHeight - h) * 0.5f, w, h},
                       gfx::kFullUv, Color{1, 1, 1, 1});
        }

        if (state == media::PlayerState::failed)
        {
            const Rect card{gfx::kVirtualWidth * 0.5f - 460, 360, 920, 260};
            list.rounded_rect(card, 28, Color::rgb(0x15161f));
            list.bordered_rect(card, 28, Color{0, 0, 0, 0}, 1.5f, kAccent.with_alpha(0.6f));
            ui::text(list, fonts.display, tr(Text::wont_play), card.cx(), card.y + 92, 40, kInk,
                     gfx::Align::center);
            ui::paragraph(list, fonts.regular,
                          svc_->player->error().empty() ? std::string(tr(Text::file_not_opened))
                                                        : svc_->player->error(),
                          card.x + 60, card.y + 150, 24, card.w - 120, 34, kInk.with_alpha(0.75f),
                          3);
            const ui::Hint hints[] = {{ui::Button::circle, tr(Text::hint_back)}};
            ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, 1, 1824, true);
            return;
        }
        if (state == media::PlayerState::opening || state == media::PlayerState::idle ||
            state == media::PlayerState::buffering)
        {
            const float cx = gfx::kVirtualWidth * 0.5f;
            if (player->texture() == 0)
            {
                ui::text(list, fonts.display, fit(fonts.display, m.title, 56, 1400), cx, 480, 56,
                         kInk, gfx::Align::center);
                ui::text(list, fonts.regular, tr(Text::opening), cx, 540, 26, kInk.with_alpha(0.6f),
                         gfx::Align::center);
            }
            list.arc(cx, 640, 26, 5, clock_ * 5.5f, 4.4f, kInk.with_alpha(0.9f));
        }

        // Subtitles sit above the controls and stay when they hide.
        if (!subtitles_.empty())
            if (const media::Cue *cue = subtitles_.at(player->position()))
                draw_subtitle(list, *cue);

        const float show = osd_show_.value;
        if (show > 0.01f && state != media::PlayerState::opening)
        {
            list.push_opacity(show);
            list.gradient_rect({0, 0, gfx::kVirtualWidth, 260}, 0, Color::rgb(0x000000, 0.8f),
                               Color::rgb(0x000000, 0.0f));
            list.gradient_rect({0, 760, gfx::kVirtualWidth, 320}, 0, Color::rgb(0x000000, 0.0f),
                               Color::rgb(0x000000, 0.88f));
            ui::text(list, fonts.display, fit(fonts.display, m.title, 44, 1300), kMargin, 112, 44,
                     kInk);
            ui::text(list, fonts.regular, meta_line(m), kMargin, 152, 24, kInk.with_alpha(0.7f));
            if (info_ && !player->summary().empty())
                ui::text(list, fonts.mono, player->summary(), kMargin, 192, 22,
                         kInk.with_alpha(0.8f));

            const double position = player->position();
            const double duration = player->duration();
            const Rect bar{kMargin, 900, gfx::kVirtualWidth - 2 * kMargin, 8};
            list.rounded_rect(bar, 4, kInk.with_alpha(0.25f));
            const float fraction =
                duration > 0.0 ? static_cast<float>(std::clamp(position / duration, 0.0, 1.0))
                               : 0.0f;
            list.rounded_rect({bar.x, bar.y, std::max(8.0f, bar.w * fraction), bar.h}, 4, kAccent);
            list.glow({bar.x + bar.w * fraction - 14, bar.y - 10, 28, 28}, 14, 18,
                      kAccent.with_alpha(0.5f));
            list.circle(bar.x + bar.w * fraction, bar.cy(), 14, kInk);
            ui::text(list, fonts.mono, format_time(position), kMargin, 954, 24, kInk);
            if (duration > 0.0)
                ui::text(list, fonts.mono, "-" + format_time(duration - position),
                         gfx::kVirtualWidth - kMargin, 954, 24, kInk.with_alpha(0.75f),
                         gfx::Align::right);

            // Pause icon in the middle while paused.
            if (state == media::PlayerState::paused)
            {
                const float cx = gfx::kVirtualWidth * 0.5f;
                list.circle(cx, 540, 70, Color::rgb(0x000000, 0.55f));
                list.rounded_rect({cx - 24, 540 - 28, 16, 56}, 4, kInk);
                list.rounded_rect({cx + 8, 540 - 28, 16, 56}, 4, kInk);
            }
            const ui::Hint hints[] = {
                {ui::Button::cross, tr(paused_ ? Text::button_resume : Text::hint_pause)},
                {ui::Button::dpad, tr(Text::hint_seek)},
                {ui::Button::square, tr(Text::hint_audio)},
                {ui::Button::triangle, tr(Text::hint_subtitles)},
                {ui::Button::circle, tr(Text::hint_stop)}};
            ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, 5, 1824, true);
            list.pop_opacity();
        }
        if (seek_flash_ > 0.0f)
        {
            const float a = tween::clamp01(seek_flash_ * 3.0f);
            char text[24];
            std::snprintf(text, sizeof(text), "%s%d s", seek_amount_ > 0 ? "+" : "\xE2\x80\x93",
                          static_cast<int>(std::fabs(seek_amount_)));
            const float cx = gfx::kVirtualWidth * 0.5f + (seek_amount_ > 0 ? 340.0f : -340.0f);
            list.push_opacity(a);
            list.circle(cx, 540, 74, Color::rgb(0x000000, 0.55f));
            ui::text(list, fonts.semibold, text, cx, 550, 32, kInk, gfx::Align::center);
            list.pop_opacity();
        }
        draw_toast(list);
    }

    void draw_subtitle(gfx::DrawList &list, const media::Cue &cue) const
    {
        const ui::Fonts &fonts = context_.fonts;
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= cue.text.size())
        {
            const std::size_t end = cue.text.find('\n', start);
            lines.push_back(
                cue.text.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        constexpr float kSize = 42.0f;
        float y = 880.0f - static_cast<float>(lines.size() - 1) * 52.0f -
                  (osd_show_.value > 0.5f ? 40.0f : 0.0f);
        for (const std::string &line : lines)
        {
            // Outline by offset copies: legible on any picture.
            for (const float dx : {-2.0f, 2.0f})
                for (const float dy : {-2.0f, 2.0f})
                    ui::text(list, fonts.semibold, line, gfx::kVirtualWidth * 0.5f + dx, y + dy,
                             kSize, Color::rgb(0x000000, 0.9f), gfx::Align::center);
            ui::text(list, fonts.semibold, line, gfx::kVirtualWidth * 0.5f, y, kSize, kInk,
                     gfx::Align::center);
            y += 52.0f;
        }
    }

    // ------------------------------------------------------------ state

    app::Context &context_;
    media::Services fallback_; // used when the app gives the screen none
    media::Services *svc_;

    Screen screen_ = Screen::browse;
    Tab tab_ = Tab::home;
    Sort sort_ = Sort::title;
    std::uint32_t revision_ = 0;
    float exit_armed_ = 0.0f; // seconds left in which a second Back press exits
    std::string cover_note_;  // the last cover problem announced

    std::vector<Shelf> shelves_;
    int row_ = 0;
    std::vector<int> grid_;
    std::string search_; // what the Movies page is filtered by; empty: everything
    ui::InputPrompt search_prompt_;
    ui::ListView settings_list_;
    ui::Form settings_form_;
    bool settings_in_form_ = false; // which of the two has the focus
    bool settings_held_ = false;    // no fresh direction since the focus entered the form
    ui::Dialog reload_dialog_;
    int grid_focus_ = 0;
    ui::Scroller grid_scroll_;

    int shown_ = -1;
    int previous_ = -1;
    std::string focus_id_; // survives a rescan: the shelves are rebuilt around it
    tween::Timer hero_fade_;
    tween::Timer details_swap_; // the page cross-fades when L1/R1 or left/right change the film
    tween::Spring hero_;
    tween::Spring row_position_;
    ui::SpringColor tone_;
    ui::SpringRect ring_;
    ui::Pulse nudge_;
    float nudge_direction_ = 0.0f;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    float tab_age_ = 1.0f;

    tween::Spring sheet_;
    int action_ = 0;
    tween::Spring action_position_;

    int playing_ = -1;
    bool paused_ = false;
    bool info_ = false;
    std::map<std::string, std::string> sniffed_subtitles_; // file -> language its text says, or ""
    int subtitle_track_ = -1; // index into the playing film's subtitles, -1 = off
    float osd_ = 0.0f;
    tween::Spring osd_show_;
    float last_save_ = 0.0f;
    float seek_flash_ = 0.0f;
    float seek_amount_ = 0.0f;
    float seek_hold_ = 0.0f; // seconds the seek direction has been held
    float seek_gap_ = 0.0f;  // seconds since the last seek step
    Direction seek_direction_ = Direction::none;
    media::Subtitles subtitles_;

    std::size_t reload_total_ = 0; // jobs of the reload that is running; 0: none is
    std::string reload_text_;
    std::string toast_;
    float toast_time_ = 0.0f;
    // First time each poster was drawn, for its fade-in.
    mutable std::map<std::string, float> seen_;
};

} // namespace

std::unique_ptr<app::Concept> make_mediathek(app::Context &context)
{
    return std::make_unique<Mediathek>(context);
}

} // namespace hui::concepts
