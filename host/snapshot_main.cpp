// ps5-homebrew-ui - Headless host renderer: runs the tour and writes PNG files.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: hui_snapshots <assets dir> <output dir> [design id|all] [width height]
//
// Runs the same shell, designs and tour script as the console build, through
// Mesa's surfaceless EGL (llvmpipe), with a fixed 60 Hz clock. Only the frames
// the tour marks are rendered, so a full run takes seconds. With
// HUI_STRIP=<from>,<to> it instead writes every frame of the switch between
// two designs (for reviewing the transition).
//
// HUI_REEL=<dir> records instead of photographing: every third simulated frame
// is piped to ffmpeg, which writes animated WebP clips into <dir>.
//   HUI_REEL_SPLIT=1   one clip per tour picture (the Theme Lab: one per theme)
//   otherwise          one clip per design, its whole tour
//   HUI_REEL=<dir> HUI_REEL_SWITCH=1   one clip of L1/R1 cycling through all designs

#include "app/shell.hpp"
#include "app/tour.hpp"
#include "core/save_file.hpp"
#include "demo/catalog.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "gfx/video_converter.hpp"
#include "manifest.hpp"
#include "core/i18n.hpp"
#include "media/info_service.hpp"
#include "media/library.hpp"
#include "media/mediainfo.hpp"
#include "media/services.hpp"
#include "media/title.hpp"
#include "ui/fonts.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                         4,
                                         EGL_CONTEXT_MINOR_VERSION,
                                         5,
                                         EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                         EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                         EGL_NONE};
    EGLContext context =
        eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// Streams raw frames to ffmpeg and gives the finished clip its name on close
// (the name is only known once the tour reaches the picture that ends it).
class Recorder
{
  public:
    Recorder(std::string directory, int width, int height, int fps)
        : directory_(std::move(directory)), width_(width), height_(height), fps_(fps)
    {
    }
    ~Recorder()
    {
        // Frames after the last picture belong to no clip.
        if (pipe_ != nullptr)
        {
            pclose(pipe_);
            std::remove((directory_ + "/.recording.webp").c_str());
        }
    }
    void frame(const unsigned char *rgba)
    {
        if (pipe_ == nullptr)
        {
            char command[512];
            std::snprintf(command, sizeof(command),
                          "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - "
                          "-vf vflip -c:v libwebp_anim -lossless 0 -q:v 62 -compression_level 5 "
                          "-loop 0 '%s/.recording.webp'",
                          width_, height_, fps_, directory_.c_str());
            pipe_ = popen(command, "w");
            frames_ = 0;
        }
        if (pipe_ != nullptr)
        {
            std::fwrite(rgba, 1, static_cast<std::size_t>(width_) * height_ * 4, pipe_);
            ++frames_;
        }
    }
    void close(const std::string &name)
    {
        if (pipe_ == nullptr)
            return;
        pclose(pipe_);
        pipe_ = nullptr;
        const std::string from = directory_ + "/.recording.webp";
        const std::string to = directory_ + "/" + name + ".webp";
        std::rename(from.c_str(), to.c_str());
        std::fprintf(stderr, "recorded %s: %d frames\n", to.c_str(), frames_);
    }

  private:
    std::string directory_;
    int width_;
    int height_;
    int fps_;
    FILE *pipe_ = nullptr;
    int frames_ = 0;
};

// A file in a series folder of the samples: an episode, 45 minutes long rather than a film's two
// hours, so the player's times and the "Up next" card look as they would.
bool is_sample_episode(const std::string &path)
{
    return path.find("/tvshows/") != std::string::npos ||
           path.find("/series/") != std::string::npos;
}
constexpr double kFilmSeconds = 7200.0;
constexpr double kEpisodeSeconds = 2700.0;

// Two simulated players, one per length; each file goes to the one of its kind.
class SamplePlayer final : public hui::media::Player
{
  public:
    SamplePlayer(const hui::media::VideoSurface &surface, bool hdr)
        : film_(hui::media::make_simulated_player(surface, kFilmSeconds, hdr)),
          episode_(hui::media::make_simulated_player(surface, kEpisodeSeconds, hdr))
    {
    }
    void open(const std::string &path, double start_seconds) override
    {
        now().close();
        on_episode_ = is_sample_episode(path);
        now().open(path, start_seconds);
    }
    void close() override
    {
        now().close();
    }
    void update(float dt) override
    {
        now().update(dt);
    }
    void pause(bool paused) override
    {
        now().pause(paused);
    }
    void seek_by(double seconds) override
    {
        now().seek_by(seconds);
    }
    void seek_to(double seconds) override
    {
        now().seek_to(seconds);
    }
    hui::media::PlayerState state() const override
    {
        return now().state();
    }
    double position() const override
    {
        return now().position();
    }
    double duration() const override
    {
        return now().duration();
    }
    std::uint32_t texture() const override
    {
        return now().texture();
    }
    int video_width() const override
    {
        return now().video_width();
    }
    int video_height() const override
    {
        return now().video_height();
    }
    const std::string &error() const override
    {
        return now().error();
    }
    const std::string &summary() const override
    {
        return now().summary();
    }
    const std::vector<hui::media::Track> &audio_tracks() const override
    {
        return now().audio_tracks();
    }
    int audio_track() const override
    {
        return now().audio_track();
    }
    void select_audio(int index) override
    {
        now().select_audio(index);
    }

  private:
    hui::media::Player &now() const
    {
        return on_episode_ ? *episode_ : *film_;
    }
    std::unique_ptr<hui::media::Player> film_;
    std::unique_ptr<hui::media::Player> episode_;
    bool on_episode_ = false;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <assets dir> <output dir> [design id|all] [width height]\n",
                     argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string output = argv[2];
    const std::string only = argc > 3 && std::string(argv[3]) != "all" ? argv[3] : "";
    const int width = argc > 5 ? std::atoi(argv[4]) : 1920;
    const int height = argc > 5 ? std::atoi(argv[5]) : 1080;

    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    std::fprintf(stderr, "GL %s / %s\n", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    hui::gfx::set_glsl_prefix("#version 450 core\n");

    hui::gfx::Renderer renderer;
    hui::gfx::Font regular;
    hui::gfx::Font semibold;
    hui::gfx::Font display;
    hui::gfx::Font mono;
    hui::gfx::Font pixel;
    hui::gfx::Font hand;
    hui::ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, assets + "/fonts/inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, assets + "/fonts/inter-semibold.huifont", &semibold,
                   &fonts.semibold) ||
        !load_font(renderer, assets + "/fonts/montserrat-medium.huifont", &display,
                   &fonts.display) ||
        !load_font(renderer, assets + "/fonts/dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, assets + "/fonts/press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, assets + "/fonts/patrick-hand.huifont", &hand, &fonts.hand))
        return 1;
    hui::demo::Catalog catalog;
    if (!catalog.build_covers(renderer, fonts))
    {
        std::fprintf(stderr, "cover art failed\n");
        return 1;
    }

    // HUI_ICON=<file.png>: draw the 512x512 launcher icon with the kit and stop.
    if (const char *icon_path = std::getenv("HUI_ICON"))
    {
        constexpr int kIcon = 512;
        GLuint target = 0;
        GLuint storage = 0;
        glGenFramebuffers(1, &target);
        glGenRenderbuffers(1, &storage);
        glBindRenderbuffer(GL_RENDERBUFFER, storage);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kIcon, kIcon);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, storage);
        using hui::gfx::Color;
        hui::gfx::BackdropSpec spec;
        spec.mode = hui::gfx::BackdropMode::aurora;
        spec.colors[0] = Color::rgb(0x0b0e20);
        spec.colors[1] = Color::rgb(0x1b1450);
        spec.colors[2] = Color::rgb(0x3b6cff);
        spec.colors[3] = Color::rgb(0x7cf0c8);
        spec.time = 42.0f;
        hui::gfx::DrawList list;
        // Three cards fanned out, the front one focused: the app in one glance.
        list.rotated_rect({118, 150, 230, 150}, 26, -0.28f, Color::rgb(0xffffff, 0.16f));
        list.rotated_rect({150, 136, 230, 150}, 26, -0.12f, Color::rgb(0xffffff, 0.3f));
        list.shadow({170, 150, 250, 160}, 30, 40, Color::rgb(0x000000, 0.5f));
        list.glow({166, 128, 250, 160}, 30, 26, Color::rgb(0x7cf0c8, 0.55f));
        list.gradient_rect({166, 128, 250, 160}, 30, Color::rgb(0xf4f7ff), Color::rgb(0xcfd8ff));
        list.bordered_rect({160, 122, 262, 172}, 34, Color::rgb(0x000000, 0.0f), 5,
                           Color::rgb(0x7cf0c8));
        list.rounded_rect({196, 164, 96, 18}, 9, Color::rgb(0x1b1450));
        list.rounded_rect({196, 198, 160, 12}, 6, Color::rgb(0x1b1450, 0.45f));
        list.rounded_rect({196, 222, 124, 12}, 6, Color::rgb(0x1b1450, 0.45f));
        list.arc(366, 176, 30, 9, 0.0f, 4.4f, Color::rgb(0x3b6cff));
        hui::ui::text(list, fonts.display, "UI Lab", 256, 402, 82, Color::rgb(0xffffff),
                      hui::gfx::Align::center);
        hui::ui::text(list, fonts.semibold, "HOMEBREW", 256, 446, 24, Color::rgb(0x7cf0c8),
                      hui::gfx::Align::center, 9.0f);
        renderer.begin();
        renderer.backdrop(spec);
        renderer.draw(list);
        renderer.present(target, kIcon, kIcon, 512.0f, 512.0f);
        std::vector<unsigned char> icon(static_cast<std::size_t>(kIcon * kIcon * 4));
        glReadPixels(0, 0, kIcon, kIcon, GL_RGBA, GL_UNSIGNED_BYTE, icon.data());
        for (std::size_t i = 3; i < icon.size(); i += 4)
            icon[i] = 255;
        stbi_flip_vertically_on_write(1);
        return stbi_write_png(icon_path, kIcon, kIcon, 4, icon.data(), kIcon * 4) != 0 ? 0 : 1;
    }

    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;

    // Settings are not read from (or written to) a real save: a scratch folder.
    const std::string data_root = output + "/.data";
    hui::save::ensure_directory(output);
    hui::save::ensure_directory(data_root);
    std::remove((data_root + "/settings.bin").c_str());
    // The movie side: a folder of invented movies (tools/make-sample-movies.py),
    // the real poster cache, and a player that only counts time.
    hui::media::PosterCache posters([&](int w, int h, const std::uint8_t *rgba)
                                    { return renderer.batch().create_texture(w, h, rgba); },
                                    [&](std::uint32_t texture)
                                    { renderer.batch().delete_texture(texture); });
    // HUI_LANG=de renders the German texts (the console's setting on a console).
    const char *language = std::getenv("HUI_LANG");
    hui::i18n::Language chosen = hui::i18n::Language::english;
    if (language != nullptr)
        hui::i18n::parse_language(language, &chosen);
    hui::i18n::set_language(chosen);
    const bool german = chosen == hui::i18n::Language::german;
    hui::media::Services services;
    // The console fetches covers; the preview only needs the action to show.
    services.refetch_cover = [](const std::string &) { return std::string(); };
    // Extra folders as the viewer might have added them (invented); the preview keeps none.
    services.scan_folders = {{"/mnt/usb1/Films", hui::media::FolderContent::automatic},
                             {"Documentaries", hui::media::FolderContent::series}};
    services.scan_folders_changed = [] {};
    {
        const char *movies = std::getenv("HUI_MOVIES");
        const std::string folder = movies != nullptr ? movies : "build/sample-movies/movies";
        // The samples keep their series beside the films, as a drive would.
        const std::string parent = hui::media::directory_of(folder);
        services.library = hui::media::Library::from_scan(
            hui::media::walk_folders({folder, parent + "/tvshows", parent + "/series"}));
        std::fprintf(stderr, "library: %zu movies and %zu series from %s\n",
                     services.library.film_count(), services.library.show_count(), folder.c_str());
        // Descriptions as the console would have fetched them (invented).
        struct Overview
        {
            const char *title;
            const char *text;
        };
        const Overview overviews[] = {
            {"Orbital Dawn",
             "When the last relay station above the ice moon goes dark, a maintenance engineer "
             "and an out-of-work pilot have eleven days to carry a message across the system. "
             "Every port they dock at remembers a different version of the story, and the "
             "message gets heavier with each one."},
            {"Winter Cartographer",
             "A mapmaker is hired to chart a valley that the villagers insist does not change. "
             "Season after season her sketches disagree with the land, and with each other, "
             "until the only honest map she can draw is one of the people who live there."},
            {"Blue Harbor", "A quiet ferry town, a missing lighthouse keeper, and a harbour "
                            "master who has stopped answering the radio."},
        };
        const Overview german_overviews[] = {
            {"Orbital Dawn",
             "Als die letzte Relaisstation \xC3\xBC"
             "ber dem Eismond ausf\xC3\xA4llt, bleiben einer "
             "Wartungsingenieurin und einem arbeitslosen Piloten elf Tage, um eine Nachricht "
             "durch das System zu tragen. Jeder Hafen erinnert sich anders an die Geschichte, "
             "und mit jedem wird die Nachricht schwerer."},
            {"Winter Cartographer",
             "Eine Kartografin soll ein Tal vermessen, das sich nach Aussage der Dorfbewohner "
             "nie ver\xC3\xA4ndert. Saison f\xC3\xBC"
             "r Saison widersprechen ihre Skizzen dem Land und "
             "einander, bis die einzige ehrliche Karte die der Menschen ist, die dort leben."},
            {"Blue Harbor",
             "Eine ruhige F\xC3\xA4hrstadt, ein verschwundener Leuchtturmw\xC3\xA4rter "
             "und ein Hafenmeister, der nicht mehr funkt."},
        };
        services.library.edit_all(
            [&](hui::media::Movie *movie)
            {
                for (const Overview &entry : german ? german_overviews : overviews)
                    if (movie->title == entry.title)
                        movie->overview = entry.text;
            });
        // What TMDB's search would have added (invented): the user score and genres.
        const struct
        {
            const char *title;
            float rating;
            std::vector<int> genres;
        } facts[] = {{"Orbital Dawn", 7.7f, {878, 12}},     {"Winter Cartographer", 6.9f, {18, 14}},
                     {"Blue Harbor", 7.2f, {9648, 18, 53}}, {"Copper Skies", 8.1f, {878, 28}},
                     {"Velvet Static", 5.8f, {27, 53}},     {"Sundial", 6.4f, {35}},
                     {"Orbital Dusk", 6.6f, {878, 12}},     {"Orbital Eclipse", 7.0f, {878, 28}},
                     {"Orbital Zenith", 7.4f, {878, 12}},   {"Echo Valley", 6.1f, {18}},
                     {"Hollow Bell", 6.8f, {18, 27}},       {"Ember & Ash", 7.5f, {18, 10749}},
                     {"Paper Moons", 7.1f, {35, 18}},       {"Lantern Season", 6.3f, {35}},
                     {"Moss & Marble", 6.0f, {35}}};
        services.library.edit_all(
            [&](hui::media::Movie *movie)
            {
                for (const auto &entry : facts)
                    if (movie->title == entry.title)
                    {
                        movie->rating = entry.rating;
                        movie->genres = entry.genres;
                    }
            });
        const struct
        {
            const char *title;
            double fraction;
            bool watched;
        } seen[] = {{"Blue Harbor", 0.42, false},  {"Winter Cartographer", 0.17, false},
                    {"Copper Skies", 0.71, false}, {"Velvet Static", 0.05, false},
                    {"Sundial", 0.0, true},        {"Paper Moons", 0.0, true}};
        std::int64_t stamp = 1700000000;
        for (const auto &entry : seen)
            for (const hui::media::Movie &movie : services.library.movies())
                if (movie.title == entry.title)
                {
                    if (entry.watched)
                        services.progress.set_watched(movie.id, true, stamp);
                    else
                        services.progress.update(movie.id, entry.fraction * 7200.0, 7200.0, stamp);
                    stamp += 100;
                }

        // The series as TMDB would have described them (invented, like the films).
        const struct
        {
            const char *folder; // the title the folder gives
            const char *title;  // TMDB's
            float rating;
            std::vector<int> genres;
            const char *english;
            const char *german;
        } shows[] = {
            {"Tidewater County",
             "Tidewater County",
             7.9f,
             {9648, 18},
             "In a ferry town where the tide decides who comes and goes, a new harbour constable "
             "finds that every family keeps its own tide table, and none of them agree on the "
             "night the lighthouse went dark.",
             "In einer F\xC3\xA4hrstadt, in der die Gezeiten bestimmen, wer kommt und geht, merkt "
             "eine neue Hafenpolizistin, dass jede Familie ihren eigenen Gezeitenkalender "
             "f\xC3\xBChrt, und keiner passt zu der Nacht, in der der Leuchtturm erlosch."},
            {"Starfall Station",
             "Starfall Station",
             8.3f,
             {878, 12},
             "Four hundred people live on a station built around a dying star. When its oldest "
             "engineer vanishes during a shift change, her apprentice starts reading the "
             "maintenance logs nobody else reads.",
             "Vierhundert Menschen leben auf einer Station rund um einen sterbenden Stern. Als "
             "die \xC3\xA4lteste Ingenieurin beim Schichtwechsel verschwindet, liest ihr "
             "Lehrling die Wartungsprotokolle, die sonst niemand liest."},
            {"The Clockmakers Guild",
             "The Clockmaker's Guild",
             6.8f,
             {35, 10751},
             "An apprentice clockmaker in a city that runs exactly four minutes late.",
             "Ein Uhrmacherlehrling in einer Stadt, die genau vier Minuten nachgeht."},
        };
        const struct
        {
            const char *show;
            int season;
            const char *english;
            const char *german;
        } season_texts[] = {
            {"Tidewater County", 1,
             "Constable Ines Marrow arrives on the last ferry of the year and inherits a case "
             "nobody wants reopened.",
             "Polizistin Ines Marrow kommt mit der letzten F\xC3\xA4hre des Jahres und erbt "
             "einen Fall, den niemand wieder aufrollen will."},
            {"Tidewater County", 2,
             "Spring brings the visitors back, and with them a stranger who knows too much about "
             "the old lighthouse.",
             "Der Fr\xC3\xBChling bringt die Besucher zur\xC3\xBC"
             "ck, und mit ihnen einen Fremden, der zu viel \xC3\xBC"
             "ber den alten Leuchtturm wei\xC3\x9F."},
            {"Starfall Station", 2,
             "The station drifts off its orbit, and the crew has to choose what to keep running.",
             "Die Station verl\xC3\xA4sst ihre Bahn, und die Besatzung muss w\xC3\xA4hlen, was "
             "weiterl\xC3\xA4uft."},
        };
        const struct
        {
            const char *show;
            int season;
            int number;
            const char *title; // "": the file name's
            const char *english;
            const char *german;
        } episode_texts[] = {
            {"Tidewater County", 1, 1, "",
             "The new constable misses the ferry back and spends her first night in the harbour "
             "master's spare room.",
             "Die neue Polizistin verpasst die F\xC3\xA4hre zur\xC3\xBC"
             "ck und verbringt die erste Nacht im G\xC3\xA4stezimmer des Hafenmeisters."},
            {"Tidewater County", 1, 3, "",
             "A birdwatcher's notebook turns up in the marsh, its last page torn out.",
             "Im Marschland taucht das Notizbuch eines Vogelkundlers auf, die letzte Seite "
             "herausgerissen."},
            {"Tidewater County", 1, 4, "",
             "Ines climbs the lighthouse at low tide and finds that the lamp was switched off "
             "by hand.",
             "Bei Ebbe steigt Ines auf den Leuchtturm und findet heraus, dass die Lampe von Hand "
             "ausgeschaltet wurde."},
            {"Tidewater County", 2, 1, "",
             "The ferry timetable changes, and so does the harbour master's story.",
             "Der F\xC3\xA4hrplan \xC3\xA4ndert sich, und mit ihm die Geschichte des "
             "Hafenmeisters."},
            {"Starfall Station", 1, 1, "Shift Change", "", ""},
            {"Starfall Station", 1, 2, "Dead Reckoning", "", ""},
            {"Starfall Station", 1, 3, "Cold Start", "", ""},
            {"Starfall Station", 2, 1, "Perihelion", "", ""},
            {"Starfall Station", 2, 2, "Ballast",
             "With the reactor at half power, the apprentice has to decide which deck goes dark "
             "first.",
             "Mit dem Reaktor auf halber Leistung muss der Lehrling entscheiden, welches Deck "
             "zuerst dunkel wird."},
        };
        services.library.edit_all(
            [&](hui::media::Movie *movie)
            {
                if (!movie->show)
                    return;
                for (const auto &entry : shows)
                    if (movie->file_title == entry.folder)
                    {
                        movie->title = entry.title;
                        movie->rating = entry.rating;
                        movie->genres = entry.genres;
                        movie->overview = german ? entry.german : entry.english;
                    }
                for (hui::media::Season &season : movie->seasons)
                {
                    for (const auto &entry : season_texts)
                        if (movie->file_title == entry.show && season.number == entry.season)
                            season.overview = german ? entry.german : entry.english;
                    for (hui::media::Episode &episode : season.episodes)
                        for (const auto &entry : episode_texts)
                            if (movie->file_title == entry.show && season.number == entry.season &&
                                episode.number == entry.number)
                            {
                                if (entry.title[0] != '\0')
                                    episode.title = entry.title;
                                episode.overview = german ? entry.german : entry.english;
                            }
                }
            });
        services.library.sort();
        const auto episode_id = [&](const char *show, int season, int number) -> std::string
        {
            for (const hui::media::Movie &movie : services.library.movies())
                if (movie.show && movie.file_title == show)
                    for (const hui::media::Season &part : movie.seasons)
                        for (const hui::media::Episode &episode : part.episodes)
                            if (part.number == season && episode.number == number)
                                return episode.id;
            return {};
        };
        // Where the viewer is: Tidewater County in its first season, Starfall Station after the
        // first episode of the second.
        const struct
        {
            const char *show;
            int season;
            int number;
            double fraction; // 0: watched
            std::int64_t stamp;
        } episodes_seen[] = {
            {"Tidewater County", 1, 1, 0.0, 1700000010},
            {"Tidewater County", 1, 2, 0.0, 1700000020},
            {"Tidewater County", 1, 3, 0.35, 1700000050},
            {"Starfall Station", 1, 1, 0.0, 1700000110},
            {"Starfall Station", 1, 2, 0.0, 1700000120},
            {"Starfall Station", 1, 3, 0.0, 1700000130},
            {"Starfall Station", 2, 1, 0.0, 1700000150},
        };
        for (const auto &entry : episodes_seen)
        {
            const std::string id = episode_id(entry.show, entry.season, entry.number);
            if (entry.fraction <= 0.0)
                services.progress.set_watched(id, true, entry.stamp);
            else
                services.progress.update(id, entry.fraction * kEpisodeSeconds, kEpisodeSeconds,
                                         entry.stamp);
        }
        // Stopped in the closing titles of S1 E4, which the store would count as watched: the
        // tour plays it from there to picture the "Up next" card of its last seconds.
        const std::string credits = episode_id("Tidewater County", 1, 4);
        if (!credits.empty())
        {
            char line[96];
            std::snprintf(line, sizeof(line), "%s %.1f %.1f %lld 0\n", credits.c_str(),
                          kEpisodeSeconds - 26.0, kEpisodeSeconds, 1700000005LL);
            services.progress.decode(services.progress.encode() + line);
        }
    }
    services.posters = &posters;
    services.clock = [] { return std::int64_t{1700000000}; };
    // What a probe of the files would have found (invented), asked for in advance so a
    // picture never shows the "reading" state by chance.
    const std::string info_directory = data_root + "/info";
    // A series has no file of its own: its episodes are probed.
    std::vector<std::pair<std::string, std::string>> files; // id, path
    for (const hui::media::Movie &movie : services.library.movies())
        if (!movie.show)
            files.emplace_back(movie.id, movie.path);
        else
            for (const hui::media::Season &season : movie.seasons)
                for (const hui::media::Episode &episode : season.episodes)
                    files.emplace_back(episode.id, episode.path);
    for (const auto &[id, path] : files)
        std::remove((info_directory + "/" + id + ".info").c_str());
    hui::media::InfoService info(
        [](const std::string &path, hui::media::MediaInfo *out)
        {
            hui::media::MediaInfo sample;
            sample.duration = 8040.0;
            sample.video_codec = "HEVC";
            sample.width = 1920;
            sample.height = 800;
            sample.container = "MKV";
            sample.audio_format = "E-AC3 5.1";
            sample.audio_languages = {"eng", "ger"};
            if (path.find("Blue Harbor") != std::string::npos)
            {
                sample.duration = 6300.0;
                sample.video_codec = "H.264";
                sample.audio_format = "AAC 2.0";
                sample.audio_languages = {"eng"};
            }
            else if (path.find("Copper Skies") != std::string::npos)
            {
                sample.video_codec = "HEVC";
                sample.width = 3840;
                sample.height = 1600;
                sample.audio_languages = {"ger", "eng", "jpn"};
            }
            if (is_sample_episode(path))
            {
                sample.duration = kEpisodeSeconds;
                sample.width = 1920;
                sample.height = 1080;
                if (path.find("2160p") != std::string::npos)
                {
                    sample.width = 3840;
                    sample.height = 2160;
                }
            }
            if (path.find("Quiet Orchard") != std::string::npos)
                return false; // a file the probe cannot read
            *out = sample;
            return true;
        },
        info_directory);
    for (const auto &[id, path] : files)
        info.request(id, path);
    for (int waited = 0; waited < 500; ++waited)
    {
        bool all = true;
        for (const auto &file : files)
            all = all && info.peek(file.first).state != hui::media::InfoService::State::pending;
        if (all)
            break;
        usleep(10 * 1000);
    }
    services.info = &info;
    hui::media::VideoSurface surface;
    surface.create = [&](int w, int h, const std::uint8_t *rgba)
    { return renderer.batch().create_texture(w, h, rgba); };
    surface.update = [&](std::uint32_t t, int w, int h, const std::uint8_t *rgba)
    { renderer.batch().update_texture(t, w, h, rgba); };
    surface.release = [&](std::uint32_t t) { renderer.batch().delete_texture(t); };
    hui::gfx::VideoConverter video_converter;
    surface.update_yuv = [&](std::uint32_t t, int w, int h, const hui::media::YuvFrame &frame)
    { return video_converter.convert(frame, t, w, h); };
    // HUI_SIM_HDR=1: the player screen shows an HDR test card through the GPU converter.
    const char *sim_hdr = std::getenv("HUI_SIM_HDR");
    SamplePlayer player(surface, sim_hdr != nullptr && std::string(sim_hdr) == "1");
    services.player = &player;

    hui::app::Shell shell(fonts, catalog, data_root, renderer.glass_texture(), &services);
    shell.set_version("host");

    constexpr float kDt = 1.0f / 60.0f;
    // HUI_MANIFEST=<file.json>: describe every design and theme (host/manifest.cpp).
    if (const char *manifest = std::getenv("HUI_MANIFEST"))
        return hui::host::write_manifest(manifest, shell) ? 0 : 1;

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
    stbi_flip_vertically_on_write(1);
    bool ok = true;
    const auto render = [&](const std::string &name)
    {
        // Pictures are decoded on a worker thread: let the ones in flight land.
        for (int tries = 0; tries < 400 && posters.pending() > 0; ++tries)
        {
            shell.rehearse();
            posters.poll();
            usleep(5000);
        }
        shell.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(framebuffer, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        // The PNG is opaque whatever the blending left in the alpha channel.
        for (std::size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        const std::string path = output + "/" + name + ".png";
        ok = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0 && ok;
        std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls, GL error 0x%x\n", path.c_str(),
                     renderer.last_instances(), renderer.last_draw_calls(), glGetError());
    };

    if (const char *strip = std::getenv("HUI_STRIP"))
    {
        int from = 0;
        int to = 1;
        std::sscanf(strip, "%d,%d", &from, &to);
        hui::InputFrame idle;
        idle.connected = true;
        shell.show(static_cast<std::size_t>(from), false);
        for (int frame = 0; frame < 120; ++frame)
            shell.update(idle, kDt);
        shell.show(static_cast<std::size_t>(to), true);
        for (int frame = 0; frame < 36; ++frame)
        {
            shell.update(idle, kDt);
            char name[32];
            std::snprintf(name, sizeof(name), "strip-%02d", frame);
            render(name);
        }
        return ok ? 0 : 1;
    }

    if (const char *reel = std::getenv("HUI_REEL"))
    {
        constexpr int kEvery = 3; // 60 Hz simulated, 20 frames per second recorded
        hui::save::ensure_directory(reel);
        Recorder recorder(reel, width, height, 60 / kEvery);
        const auto record = [&]()
        {
            shell.compose(renderer);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(framebuffer, width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            recorder.frame(pixels.data());
        };
        hui::InputFrame idle;
        idle.connected = true;
        long count = 0;
        if (std::getenv("HUI_REEL_SWITCH") != nullptr)
        {
            // R1 through every design, then the info panel: the app in one clip.
            shell.show(0, false);
            for (std::size_t i = 0; i <= shell.concept_count(); ++i)
            {
                for (int frame = 0; frame < 66; ++frame, ++count)
                {
                    hui::InputFrame input = idle;
                    if (frame == 0 && i > 0)
                        input.pressed = hui::action_bit(hui::Action::page_next);
                    shell.update(input, kDt);
                    if (count % kEvery == 0)
                        record();
                }
            }
            recorder.close("switcher");
            return 0;
        }
        const bool split = std::getenv("HUI_REEL_SPLIT") != nullptr;
        hui::app::Tour tour(shell, only);
        std::size_t design = shell.current();
        char name[96];
        while (!tour.finished() && count < 60 * 60 * 60)
        {
            const hui::InputFrame input = tour.step(kDt);
            if (!split && shell.current() != design)
            {
                std::snprintf(name, sizeof(name), "%s", shell.concept_at(design).info().id);
                recorder.close(name);
                design = shell.current();
            }
            shell.update(input, kDt);
            if (count % kEvery == 0)
                record();
            ++count;
            if (!tour.capture().empty())
            {
                if (split)
                    recorder.close(tour.capture());
                tour.capture_done();
            }
        }
        if (!split)
            recorder.close(shell.concept_at(design).info().id);
        return 0;
    }

    hui::app::Tour tour(shell, only);
    if (tour.finished())
    {
        std::fprintf(stderr, "no design named '%s'\n", only.c_str());
        return 2;
    }
    long frames = 0;
    while (!tour.finished() && frames < 60 * 60 * 60)
    {
        const hui::InputFrame input = tour.step(kDt);
        // Nothing is presented here, so the numbers a console would measure
        // are stood in for: 60 Hz with a little jitter and a rare long frame.
        hui::app::Telemetry &telemetry = shell.telemetry();
        const float jitter = static_cast<float>((frames * 7919) % 97) / 97.0f;
        telemetry.push(16.3f + jitter * 0.8f + (frames % 211 == 0 ? 5.5f : 0.0f));
        telemetry.fps = 59.9f;
        telemetry.average_ms = 16.7f;
        telemetry.voices = static_cast<int>(frames / 40 % 4);
        telemetry.draw_calls = renderer.last_draw_calls();
        telemetry.instances = renderer.last_instances();
        posters.poll();
        shell.update(input, kDt);
        ++frames;
        if (!tour.capture().empty())
        {
            render(tour.capture());
            tour.capture_done();
        }
        else
        {
            shell.rehearse(); // as on the console, every frame is drawn
            if (posters.pending() > 0)
                usleep(1500);
        }
    }
    std::fprintf(stderr, "tour: %ld frames simulated\n", frames);
    return ok ? 0 : 1;
}
