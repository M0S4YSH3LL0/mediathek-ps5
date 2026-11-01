// Mediathek - Application entry point.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and audio, then runs the shell every frame:
// read input, update, play the sounds it asked for, draw, present. Lifecycle
// markers and frame pacing are logged for hardware runs.

#include "app/shell.hpp"
#include "app/tour.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "audio/music.hpp"
#include "core/frame_stats.hpp"
#include "core/i18n.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "core/version.hpp"
#include "demo/catalog.hpp"
#include "gfx/canvas.hpp"
#include "gfx/renderer.hpp"
#include "media/cover_service.hpp"
#include "media/library.hpp"
#include "media/poster_cache.hpp"
#include "media/progress.hpp"
#include "media/services.hpp"
#include "media/tmdb.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/dns.h"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/ffmpeg_player.hpp"
#include "platform/ps5/http.hpp"
#include "platform/ps5/media_probe.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/scanner.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <cstring>
#include <ctime>
#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

extern "C" void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes,
                               std::size_t *blocks, std::size_t *failures);
extern "C" std::size_t hui_heap_capacity(void);

namespace
{

constexpr const char *kAssets = "/app0/assets";
// Settings live in the title's own save area.
constexpr const char *kDataRoot = "/download0/hui";
// Resume points and watched marks, in a checked container like the settings.
constexpr const char *kProgressPath = "/download0/hui/progress.bin";
constexpr std::uint16_t kProgressVersion = 1;
// The owner's settings for fetching covers (TMDB_API_KEY=...), beside the app
// where a PC can write it; and where the fetched pictures are kept.
constexpr const char *kEnvPath = "/app0/mediathek.env";
constexpr const char *kCoverDir = "/download0/hui/covers";
// What each film file contains, read once (length, codecs, languages).
constexpr const char *kInfoDir = "/download0/hui/info";
// New pictures reach the screen at most this often: each one rebuilds the shelves.
constexpr double kCoverApplySeconds = 1.5;
// Saved at most this often while something changes; the player also marks its
// position every few seconds, so a closed title loses a few seconds at most.
constexpr double kProgressSaveSeconds = 3.0;
// The mixer stream a movie's sound goes through: 2.7 s of 48 kHz stereo.
constexpr std::size_t kMovieRingFrames = 1u << 17;
// Development output (the tour's pictures and report, the log). A title's
// sandbox has no /data, so it lives in the title's own storage. While the
// title runs, a PC can *read* the folder over FTP as
// /mnt/sandbox/<TITLE_ID>_000/download0/hui/dev, but cannot write there.
constexpr const char *kDevRoot = "/download0/hui/dev";
// Requests therefore come in through the install folder, which a PC can
// write: /data/homebrew/<TITLE_ID>/dev/request.txt is /app0/dev/request.txt
// here. One line: "tour <design id|all> <token>" or "quit - <token>". A
// request is honoured once per token (the last one is remembered in kDevRoot).
constexpr const char *kRequestPath = "/app0/dev/request.txt";
// How long a finished tour stays up so the PC can copy its evidence (the
// storage is unmounted when the title closes) before the app closes itself.
constexpr double kCollectSeconds = 240.0;
// Quarter of a quarter: the title's storage is small and a tour takes
// hundreds of pictures. Enough to compare layout and colour with the PC's.
constexpr int kCaptureWidth = 480;
constexpr int kCaptureHeight = 270;

void log_heap(std::uint64_t frames)
{
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::size_t failures = 0;
    hui_heap_stats(&live, &peak, &blocks, &failures);
    hui::sys::log("[HUI] heap frames=%llu live=%zu peak=%zu of=%zu blocks=%zu failures=%zu",
                  static_cast<unsigned long long>(frames), live, peak, hui_heap_capacity(), blocks,
                  failures);
}

// Opens at *resolution (an index into Settings::kResolutions), falling back
// to 1080p; *resolution reports the mode that opened.
bool open_display(hui::ps5::Display &display, int *resolution)
{
    using hui::Settings;
    const Settings::Resolution &mode = Settings::kResolutions[*resolution];
    if (display.open(mode.width, mode.height))
    {
        hui::sys::log("[HUI] display mode %s %dx%d", mode.label, display.width(), display.height());
        return true;
    }
    if (*resolution == 0)
        return false;
    hui::sys::log("[HUI] display mode %s failed, using 1080p", mode.label);
    *resolution = 0;
    return display.open(Settings::kResolutions[0].width, Settings::kResolutions[0].height);
}

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[HUI] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// The frame as drawn, bottom row first, as a 24-bit BMP (tour pictures).
bool save_picture(const std::string &path, int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u;
    const std::uint32_t size = 54 + row * static_cast<std::uint32_t>(height);
    unsigned char header[54] = {'B', 'M'};
    const auto put = [&](int at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, static_cast<std::uint32_t>(width));
    put(22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; ok && y < height; ++y)
    {
        const unsigned char *in = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x)
        {
            line[static_cast<std::size_t>(x) * 3 + 0] = in[x * 4 + 2];
            line[static_cast<std::size_t>(x) * 3 + 1] = in[x * 4 + 1];
            line[static_cast<std::size_t>(x) * 3 + 2] = in[x * 4 + 0];
        }
        ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
    }
    return std::fclose(file) == 0 && ok;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[HUI] entry");
    sys::log("[HUI] storage dir=%d", save::ensure_directory(kDataRoot) ? 1 : 0);

    // The owner's settings (mediathek.env, beside the app) and the language,
    // before anything that shows or fetches text: the console's own language
    // unless UI_LANGUAGE in that file says otherwise; English where unknown. A language chosen
    // on the Settings page wins over both.
    std::string env_text;
    save::read_file(kEnvPath, &env_text, 16u << 10);
    const media::Env env = media::parse_env(env_text);
    const Settings saved = app::Shell::load_settings(kDataRoot);
    const i18n::Language automatic_language = [&]
    {
        const int console = sys::console_language();
        i18n::Language chosen = i18n::from_console_language(console);
        const std::string wanted = env.get("UI_LANGUAGE");
        if (!wanted.empty())
            i18n::parse_language(wanted, &chosen); // "auto" and typos keep the console's
        const i18n::Language ui = i18n::choose_language(saved.ui_language, chosen);
        i18n::set_language(ui);
        sys::log("[HUI] language console=%d env=%s settings=%d ui=%s", console,
                 wanted.empty() ? "auto" : wanted.c_str(), saved.ui_language,
                 ui == i18n::Language::german ? "de" : "en");
        return chosen;
    }();

    // The display opens at the saved resolution (1080p if that fails).
    int resolution = ps5::Display::supports_display_modes() ? saved.resolution : 0;
    ps5::Display display;
    if (!open_display(display, &resolution))
    {
        sys::log("[HUI] fatal: display open failed");
        sys::park();
    }

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    gfx::Font pixel;
    gfx::Font hand;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "montserrat-medium.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, "press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, "patrick-hand.huifont", &hand, &fonts.hand))
    {
        sys::log("[HUI] fatal: renderer init failed");
        sys::park();
    }
    demo::Catalog catalog;
    const std::int64_t art_start = sys::monotonic_us();
    if (!catalog.build_covers(renderer, fonts))
        sys::log("[HUI] cover art failed; designs draw without it");
    sys::log("[HUI] cover art %zu covers in %lld ms", catalog.size(),
             static_cast<long long>((sys::monotonic_us() - art_start) / 1000));

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    audio::Mixer mixer;
    // The music stream attaches to the mixer before the audio thread starts.
    audio::MusicPlayer music;
    const int tracks = music.init(mixer, std::string(kAssets) + "/audio/music",
                                  static_cast<std::uint64_t>(sys::monotonic_us()));
    sys::log("[HUI] music songs=%d", tracks);
    // The movie's sound ring attaches before the audio thread starts, too.
    audio::StreamRing movie_ring(kMovieRingFrames);
    mixer.attach_stream(audio::Mixer::kMovieStream, &movie_ring);
    mixer.set_stream_gain(audio::Mixer::kMovieStream, 1.0f, 0.0f);
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(std::string(kAssets) + "/audio/sfx");
    sys::log("[HUI] sounds files=%d rejected=%d", bank.files, bank.rejected);
    for (const std::string &error : bank.errors)
        sys::log("[HUI] sound rejected %s", error.c_str());

    // The movie side: library, artwork, resume points, player.
    media::Services services;
    services.automatic_language = automatic_language;
    services.clock = [] { return static_cast<std::int64_t>(std::time(nullptr)); };
    {
        std::string data;
        if (save::read_file(kProgressPath, &data))
        {
            const save::Decoded decoded = save::decode(save::Kind::library, data);
            if (!decoded.ok || !services.progress.decode(decoded.payload))
                sys::log("[HUI] progress unreadable (%s), starting empty", decoded.error.c_str());
        }
        sys::log("[HUI] progress entries=%zu", services.progress.size());
        services.progress.mark_saved();
    }
    media::PosterCache posters([&renderer](int w, int h, const std::uint8_t *rgba)
                               { return renderer.batch().create_texture(w, h, rgba); },
                               [&renderer](std::uint32_t texture)
                               { renderer.batch().delete_texture(texture); });
    services.posters = &posters;
    media::VideoSurface surface;
    surface.create = [&renderer](int w, int h, const std::uint8_t *rgba)
    { return renderer.batch().create_texture(w, h, rgba); };
    surface.update = [&renderer](std::uint32_t texture, int w, int h, const std::uint8_t *rgba)
    { renderer.batch().update_texture(texture, w, h, rgba); };
    surface.release = [&renderer](std::uint32_t texture)
    { renderer.batch().delete_texture(texture); };
    const std::unique_ptr<media::Player> player = ps5::make_ffmpeg_player(
        surface, &movie_ring, [&mixer](bool on)
        { mixer.set_stream_gain(audio::Mixer::kMovieStream, on ? 1.0f : 0.0f, 0.04f); });
    services.player = player.get();

    // Covers and wallpapers from TMDB, fetched in the background for every film
    // that has none of its own.
    const media::TmdbConfig tmdb = media::config_from_env(env);
    mt_dns_set_server(tmdb.dns.c_str());
    sys::log("[HUI] covers key=%d language=%s dns=%s %s", tmdb.usable() ? 1 : 0,
             tmdb.language.c_str(), tmdb.dns.empty() ? "default" : tmdb.dns.c_str(),
             tmdb.problem.c_str());
    media::CoverService covers(tmdb, ps5::make_http_get(std::string(kAssets) + "/certs/cacert.pem"),
                               kCoverDir);
    media::InfoService info(ps5::make_media_probe(), kInfoDir);
    services.info = &info;
    services.refetch_cover = [&](const std::string &id) -> std::string
    {
        const int index = services.library.find(id);
        if (index < 0)
            return i18n::tr(i18n::Text::unknown_movie);
        const media::Movie &movie = services.library[static_cast<std::size_t>(index)];
        // A picture that sits beside the film is the owner's choice: leave it.
        const std::string directory = std::string(kCoverDir) + "/";
        const auto ours = [&](const std::string &path)
        { return path.empty() || path.compare(0, directory.size(), directory) == 0; };
        media::CoverJob job;
        job.id = movie.id;
        job.title = movie.title;
        job.year = movie.year;
        job.want_poster = ours(movie.poster);
        job.want_wallpaper = ours(movie.backdrop);
        job.want_overview = true; // the description is always ours
        job.again = true;
        info.refresh(movie.id, movie.path); // the file too: it may have been replaced
        if (!tmdb.usable())
            return tmdb.problem;
        covers.redo(job);
        return std::string();
    };

    ps5::Scanner scanner;
    const auto adopt = [&](const media::ScanResult &scan)
    {
        services.library = media::Library::from_scan(scan);
        services.library.edit_all([&](media::Movie *movie) { covers.store().fill(movie); });
        covers.enqueue(media::missing_covers(services.library, covers.store()));
        posters.clear();
        ++services.revision;
        sys::log("[HUI] library movies=%zu roots=%zu", services.library.size(), scan.roots.size());
    };
    // Looks for the movies: the folders directly (a console that allows
    // listing), else through the helper payload, whose answer arrives later.
    const auto scan = [&]()
    {
        const media::ScanResult direct = media::walk_folders(media::default_roots());
        if (!direct.videos.empty())
        {
            adopt(direct);
            services.scanning = false;
            services.scan_note.clear();
            return;
        }
        services.scanning = scanner.start();
        services.scan_note = scanner.note();
        sys::log("[HUI] scan helper started=%d %s", services.scanning ? 1 : 0,
                 scanner.note().c_str());
    };
    services.rescan = scan;
    {
        media::ScanResult cached;
        if (ps5::Scanner::load_cached(&cached))
            adopt(cached);
    }
    scan();

    app::Shell shell(fonts, catalog, kDataRoot, renderer.glass_texture(), &services);
    // The only sound setting is the switch on the settings page: menu sounds on or off. The
    // levels stay as they were tuned (the music bus is not touched), and the saved choice
    // has to be applied at start-up too, not only when it changes.
    const auto apply_ui_sounds = [&mixer](const Settings &settings)
    {
        const float level = settings.ui_volume > 0 ? 1.0f : 0.0f;
        mixer.set_bus_gain(audio::Bus::ui, level);
        mixer.set_bus_gain(audio::Bus::sfx, level);
    };
    apply_ui_sounds(shell.settings());
    const std::string version = read_content_version("/app0/sce_sys/param.json");
    shell.set_version(version);
    sys::log("[HUI] version %s designs=%zu", version.empty() ? "unknown" : version.c_str(),
             shell.concept_count());

    // A tour request turns this launch into an unattended validation run:
    // every design is shown with scripted input, pictures and a report are
    // written to kDevRoot, and the app closes itself a while later.
    std::unique_ptr<app::Tour> tour;
    save::ensure_directory(kDevRoot);
    const std::string handled_path = std::string(kDevRoot) + "/handled.txt";
    const std::string report_path = std::string(kDevRoot) + "/tour/report.txt";
    bool quit_requested = false;
    const auto poll_request = [&]()
    {
        std::string request;
        if (!save::read_file(kRequestPath, &request, 256))
            return;
        char verb[16] = {};
        char argument[64] = {};
        char token[64] = {};
        if (std::sscanf(request.c_str(), "%15s %63s %63s", verb, argument, token) != 3)
            return;
        std::string handled;
        save::read_file(handled_path, &handled, 128);
        if (handled == token)
            return; // this request was honoured by an earlier launch (or frame)
        save::write_atomic(handled_path, token);
        if (std::strcmp(verb, "quit") == 0)
        {
            quit_requested = true;
        }
        else if (std::strcmp(verb, "tour") == 0 && !tour)
        {
            save::ensure_directory(std::string(kDevRoot) + "/tour");
            std::remove(report_path.c_str()); // a new report must not be taken for an old one
            const bool all = std::strcmp(argument, "all") == 0;
            tour = std::make_unique<app::Tour>(shell, all ? std::string() : std::string(argument));
            sys::log("[HUI] tour requested: %s token=%s", argument, token);
        }
    };
    poll_request();
    services.quit = [&quit_requested] { quit_requested = true; };
    double collect_wait = -1.0; // seconds a finished tour has waited to be collected
    double progress_clock = 0.0;
    double cover_wait = 0.0;
    const auto save_progress = [&]()
    {
        const std::string error =
            save::write_atomic(kProgressPath, save::encode(save::Kind::library, kProgressVersion,
                                                           services.progress.encode()));
        if (!error.empty())
            sys::log("[HUI] progress not saved: %s", error.c_str());
        else
            services.progress.mark_saved();
    };

    std::int64_t previous = sys::monotonic_us();
    std::uint64_t frames = 0;
    FrameStats stats;
    // The FPS readout averages over half a second so the number is readable.
    double fps_seconds = 0.0;
    int fps_frames = 0;
    PadSample samples[64];
    gfx::Canvas capture;
    std::int64_t last_frame_start = sys::monotonic_us();
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        // Animation time is start-to-start (one full frame), not the gap
        // between the previous swap returning and this frame beginning.
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f; // a hitch must not teleport the animations
        const std::size_t count = pad.read(samples);
        InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                          static_cast<std::uint64_t>(now));
        if (tour)
            input = tour->step(dt);

        if (services.scanning)
        {
            media::ScanResult found;
            const ps5::Scanner::Status status = scanner.poll(dt, &found);
            if (status == ps5::Scanner::Status::done)
            {
                adopt(found);
                services.scan_found = static_cast<int>(services.library.size());
                services.scanning = false;
                services.scan_note.clear();
            }
            else if (status == ps5::Scanner::Status::failed)
            {
                services.scanning = false;
                services.scan_note = scanner.note();
                sys::log("[HUI] scan failed: %s", scanner.note().c_str());
            }
        }
        posters.poll();
        cover_wait += static_cast<double>(dt);
        if (cover_wait >= kCoverApplySeconds)
        {
            cover_wait = 0.0;
            const std::vector<media::CoverService::Done> arrived = covers.take_done();
            if (!arrived.empty())
            {
                bool replaced = false;
                for (const media::CoverService::Done &done : arrived)
                    replaced = replaced || done.replaced;
                services.library.edit_all([&](media::Movie *movie) { covers.store().fill(movie); });
                if (replaced)
                    posters.clear(); // a texture of the old picture may be cached
                ++services.revision;
                sys::log("[HUI] covers arrived=%zu waiting=%zu", arrived.size(), covers.pending());
            }
        }
        // In this order: a queue given up on is empty only once its problem is set, so the screen
        // never takes the end of a failed reload for a finished one.
        services.cover_jobs = covers.pending();
        services.cover_note = covers.problem();

        shell.update(input, dt);
        progress_clock += static_cast<double>(dt);
        if (services.progress.dirty() && progress_clock >= kProgressSaveSeconds)
        {
            progress_clock = 0.0;
            save_progress();
        }
        if (shell.take_settings_changed())
        {
            const Settings &settings = shell.settings();
            apply_ui_sounds(settings);
            InputSettings input_settings = tracker.settings();
            input_settings.swap_confirm = settings.swap_confirm;
            tracker.set_settings(input_settings);
        }
        const app::Feedback &feedback = shell.feedback();
        for (const audio::CueEvent &event : feedback.cues)
        {
            sounds.play(mixer, event.set == audio::SoundSet::count ? shell.sound_set() : event.set,
                        event);
            if (event.cue == audio::Cue::complete || event.cue == audio::Cue::welcome)
                music.duck();
        }
        if (shell.settings().haptics && feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
        if (shell.settings().light_bar)
        {
            const gfx::Color accent = shell.accent();
            pad.set_light_bar(static_cast<std::uint8_t>(accent.r * 255.0f),
                              static_cast<std::uint8_t>(accent.g * 255.0f),
                              static_cast<std::uint8_t>(accent.b * 255.0f));
        }
        music.pump(dt);

        shell.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (tour && !tour->capture().empty())
        {
            // The picture is the same frame drawn once more into a small
            // off-screen target: reading the display surface back is slow, and
            // a quarter-size picture is enough to compare with the PC render.
            if (capture.texture() == 0)
                capture.create(kCaptureWidth, kCaptureHeight, 1);
            capture.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(capture.framebuffer(), kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, capture.framebuffer());
            const std::string path = std::string(kDevRoot) + "/tour/" + tour->capture() + ".bmp";
            const bool ok = save_picture(path, kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            sys::log("[HUI] tour picture %s ok=%d", tour->capture().c_str(), ok ? 1 : 0);
            tour->capture_done();
            last_frame_start = sys::monotonic_us(); // saving is slow; the frame was not
        }
        if (!display.swap())
        {
            sys::log("[HUI] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        ++frames;
        const std::int64_t presented = sys::monotonic_us();
        const float frame_ms = static_cast<float>(presented - previous) / 1000.0f;
        app::Telemetry &telemetry = shell.telemetry();
        if (frames == 1)
        {
            sys::log("[HUI] first-swap ok shapes=%zu draws=%zu", renderer.last_instances(),
                     renderer.last_draw_calls());
            const bool hidden = sys::hide_splash_screen();
            sys::log("[HUI] ready splash_hidden=%d", hidden ? 1 : 0);
            log_heap(frames);
        }
        else
        {
            stats.add(static_cast<double>(frame_ms));
            telemetry.push(frame_ms);
            if (tour)
                tour->record_frame(frame_ms, renderer.last_draw_calls(), renderer.last_instances());
        }
        previous = presented;
        telemetry.draw_calls = renderer.last_draw_calls();
        telemetry.instances = renderer.last_instances();
        telemetry.voices = mixer.active_voices();
        fps_seconds += static_cast<double>(frame_ms) / 1000.0;
        ++fps_frames;
        if (fps_seconds >= 0.5)
        {
            telemetry.fps = static_cast<float>(fps_frames / fps_seconds);
            telemetry.average_ms = static_cast<float>(fps_seconds * 1000.0 / fps_frames);
            fps_seconds = 0.0;
            fps_frames = 0;
        }

        if (tour && tour->finished())
        {
            std::string report;
            for (const std::string &line : tour->report())
            {
                sys::log("[HUI] %s", line.c_str());
                report += line + "\n";
            }
            save::write_atomic(report_path, report);
            sys::log("[HUI] tour finished designs=%zu", tour->report().size());
            // The storage is unmounted when the title closes, so the app stays
            // up for a while: the PC copies the evidence, then the app closes.
            tour.reset();
            collect_wait = 0.0;
        }
        if (collect_wait >= 0.0)
            collect_wait += static_cast<double>(frame_ms) / 1000.0;
        if (frames % 60 == 0)
            poll_request();
        // Checked every frame: leaving with the Back button must not lag.
        if (quit_requested || collect_wait > kCollectSeconds)
        {
            sys::log("[HUI] closing: %s",
                     quit_requested ? "quit requested" : "tour evidence window over");
            if (services.progress.dirty())
                save_progress();
            player->close();
            pad.close();
            sys::quit();
        }
        if (stats.count() == 600)
        {
            char summary[160];
            stats.format(summary, sizeof(summary));
            sys::log("[HUI] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                     renderer.last_instances());
            sys::log("[HUI] audio grains=%llu errors=%llu voices=%d music=%s underruns=%llu",
                     static_cast<unsigned long long>(audio_out.grains()),
                     static_cast<unsigned long long>(audio_out.errors()), mixer.active_voices(),
                     music.current().c_str(),
                     static_cast<unsigned long long>(mixer.stream_underruns()));
            stats.reset();
            if (frames % 3600 < 600)
                log_heap(frames);
        }
    }
}
