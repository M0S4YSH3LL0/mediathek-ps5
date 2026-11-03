# Instructions for AI agents

This file is for coding agents (and people in a hurry). It says what this
repository is, where to look, how to work in it, and what "done" means here.

## What this is

**Mediathek**: a Netflix-style movie library and player for the PS5, a native
C++20 app drawn with OpenGL 4.6 (title id `PPSA66120`, version in
`sce_sys/param.json`). It finds films in `movies` and `mediathek/movies` folders on the internal drive,
USB and extended storage, shows a billboard, shelves and a grid, fetches covers,
wallpapers and descriptions from TMDB, plays files with FFmpeg and remembers
where you stopped. The UI is English or German.

It is built on the [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui)
kit: its component library, themes, mixer and PC preview are all still here, but
the gallery of demo designs was removed. `src/concepts/mediathek.cpp` is the one
design. Do not add another rendering path: everything on screen goes through the
kit's OpenGL renderer.

## Repository map

```
src/concepts/mediathek.cpp   the whole UI: home, grid, details page, player screen
src/main.cpp                 start-up: env file, language, library, services, cover jobs
src/media/                   the movie side, no platform code (mediainfo: languages,
                               runtime, the cache text of what a file contains, and the
                               audio language a film's name says when its file does not, and the
                               language of a subtitle file from its name or, failing that, its text;
                               info_service: reads files in the background):
                               library (scan index -> Movie list, a film's subtitle
                               files, genre shelves, search matching), title (file
                               name -> title, year, quality; search_fold), progress (resume
                               points, watched), subtitles (.srt), tmdb + cover_service
                               (poster, wallpaper, description), poster_cache,
                               hw_stream, yuv_picture (feeding the hardware decoder, its pictures to RGBA),
                               player.hpp (the interface), simulated_player (PC preview)
src/core/i18n*               texts: i18n_texts.inc is THE table (English, German)
src/platform/ps5/            ffmpeg_player, hw_decoder (the console's picture decoder), media_probe (stream list of a file, no
                             decoding), scanner (talks to the helper), display,
                             pad, audio_out, http + dns (own TLS client), system
src/runtime/app_heap.c       the app's own heap (see "Memory" below)
helper/mt-scan.c             the scan helper payload, built into assets/helper/mt-scan.elf
src/ui/, src/gfx/, src/audio/  the kit: components, themes, renderer, mixer
host/                        the PC preview (Mesa, off-screen): snapshot_main.cpp
tests/unit/                  GoogleTest, no OpenGL; sources.txt lists what is compiled
tools/                       build.sh, host-snapshots.sh, make-sample-movies.py,
                             fetch-posters.py (PC poster tool), bake-fonts.sh
sce_sys/                     what the home screen shows (the owner's artwork)
docs/                        the kit's guides; docs/screenshots are the README's pictures
```

The kit's guides still apply when you touch the UI: [docs/CRAFT.md](docs/CRAFT.md)
(quality bar, read it before any UI work), [docs/COMPONENT_INDEX.md](docs/COMPONENT_INDEX.md)
(find a component before hand-rolling one), [docs/KIT.md](docs/KIT.md),
[docs/THEMES.md](docs/THEMES.md), [docs/PERFORMANCE.md](docs/PERFORMANCE.md),
[docs/CONSOLE_VALIDATION.md](docs/CONSOLE_VALIDATION.md).

## The loop

Work on a PC first. A console is only for the final check.

```bash
export HOST_CXX=clang++-19 HOST_CC=clang-19 USE_CCACHE=0   # LLVM 19 is the default, in CI too

tools/host-snapshots.sh build/snapshots mediathek    # PC preview: PNG frames of every screen
HUI_LANG=de tools/host-snapshots.sh build/snapshots-de mediathek   # the same in German
make test-unit                                       # GoogleTest, sanitizers on
make format-check                                    # clang-format (make format fixes)
make                                                 # the PS5 app folder in dist/PPSA66120
make lint                                            # lists files with git, takes minutes
```

1. Change code.
2. Render the preview and **look at the pictures** in `build/snapshots/`. A build
   that compiles proves nothing about a UI. Look at German too when text changes:
   it is longer.
3. Run the tests, then build for the PS5 (no warnings).
4. Lint.

The preview takes the library from `build/sample-movies/movies` (made by
`python3 tools/make-sample-movies.py`; `HUI_MOVIES=<folder>` points it elsewhere)
and invents descriptions in `host/snapshot_main.cpp`. What a picture shows is
decided by the design's `tour()` (`kTour` in `mediathek.cpp`): a named picture
is taken *before* its own step's input acts. To photograph a state that has none,
change the tour temporarily and restore it. The preview has no sound, no video
(a simulated player draws a moving test picture) and no network.

`make lint` lists its files with `git ls-files` (tracked and new ones), so it needs this
folder's git repository and takes more than two minutes; use a long timeout.

## Rules

**Code**

- C++20. The PS5 build has no exceptions and no RTTI: no `try`, `throw`,
  `dynamic_cast` or `typeid`.
- No warnings. Tests build with `-Wall -Wextra -Wpedantic -Werror` under
  ASan/UBSan; lint runs clang-tidy with warnings as errors.
- Every source file starts with three header lines: name and purpose, copyright,
  SPDX identifier. Lint checks it.
- Comments explain *why* (the technique, the constraint, the trap), not what.
- `concept` is a C++20 keyword. Name variables `design`.
- `src/media/` stays free of platform and GL code so the tests can cover it.
  Platform behaviour goes behind an interface (`media::Player`, the scanner).

**Texts and languages**

- Every string the viewer can see goes through `src/core/i18n_texts.inc`:
  `HUI_TEXT(id, english, german)`. Then `tr(Text::id)`, or `i18n::format(...)`
  where a `{}` marks a time, name or number. There is no text without its German.
- UTF-8 in the table is written as hex escapes. A `\xBC` followed by a letter
  a-f swallows it as a hex digit, so split the literal: `"Zur\xC3\xBC" "ck"`.
- The language comes from the console (`sys::console_language()`: the system
  parameter `1`, German = value `4`), English otherwise; `UI_LANGUAGE=en|de` in
  `mediathek.env` overrides it ("Automatic"); *App language* on the Settings page
  (`Settings::ui_language`: 0 automatic, 1 English, 2 German) wins over both and switches
  at once (`change_language()` rebuilds the shelves, the search prompt and the form; TMDB
  descriptions follow the language at the next start). The log line
  `[HUI] language console=... env=... settings=... ui=...` says what happened. Parameter id `2` is the date format, not the language.
- The fonts are baked with a Latin / Greek / Cyrillic glyph set
  (`tools/bake-fonts.sh`, `make fonts`) so umlauts and accented film titles draw.
  Check a new text fits its button or column in German before calling it done.

**UI**

- 1920 x 1080 virtual canvas, inside the safe area (96 px at the sides). Text is
  positioned by its baseline.
- `update()` owns state and animation, driven by `dt`; `draw()` is `const` and
  pure. Springs need a `snap()` at start-up.
- Use `Action::confirm` and `Action::back`, never physical buttons. L1/R1 switch
  tabs here.
- Honour `settings.reduced_motion`; every action has a cue.
- Content is invented: no real product names, brands or artwork in code, tests,
  previews or screenshots. The sample films are made up.

**Secrets**

- The TMDB key belongs in the owner's `.env` (and from there in
  `dist/PPSA66120/mediathek.env`) or in `mediathek.env` on the console. The build
  copies only `TMDB_API_KEY`, `TMDB_LANGUAGE`, `DNS_SERVER`, `UI_LANGUAGE` and `HW_DECODE`, and
  never into the `.zip`. Never print, log or paste the key; `.env` and `dist/` are
  git-ignored.

**Assets**

- `sce_sys/` (icon, backgrounds, music) is the owner's artwork: do not replace it
  unasked. `docs/screenshots` come from the preview with invented films.
- Sound effects and music: reuse what is in `assets/audio`; do not generate new
  ones unless asked.
- Third-party code, fonts and data go into `THIRD_PARTY_NOTICES.md` with their
  licence. The project is GPL-3.0-or-later.

**Git**

- Do not commit, tag or push unless the owner asks. A release is the owner's call: the
  commit, the tag, the push and the GitHub release (zip and `.sha256` from `dist/release`).
  When asked: small commits, one-line imperative subject.
- A release tag must equal `sce_sys/param.json` `contentVersion` exactly
  (`01.000.080` is the app's 0.8.0); the app shows the short form (`kAppVersion` in
  `mediathek.cpp`). Change both, `README.md` (Status) and `CHANGELOG.md` together.
- Never commit build output, console evidence, addresses of machines, personal
  paths or credentials.

## Things that are not obvious

**A sandboxed title cannot list a folder.** The app sends `mt-scan.elf` to an ELF
loader on `127.0.0.1:9021`; it walks the `movies` and `mediathek/movies` folders of each
drive (the same list as `default_roots()` in `library.cpp`; change both), copies small pictures
and subtitles next to the app and writes `mediathek-index.txt`, which the app
reads as `/app0/mediathek-index.txt`. Without a loader the app starts and finds
nothing. The install folder `/data/homebrew/PPSA66120` is `/app0` inside the app.

**Extra folders reach the helper inside its own bytes.** The loader runs a payload with no
arguments, so `mt-scan.c` has a 20 KB `volatile` block `extra_roots` in its data that starts with
the mark `MT-EXTRA-ROOTS-1\n`; before sending, `Scanner::start` finds the mark and writes the roots
there, one a line (`media::embed_helper_roots`; the mark and the size are written in both files,
change both). The helper scans them after its own folders. The viewer's list (Settings > Library,
`Services::scan_folders`, at most 8) is kept in `/download0/hui/scan-folders.txt`, one a line: a
path starting with `/` is that folder, any other name is looked for on every drive
(`scan_folder_roots`). Two helpers at once would write the same index, so a change during a scan
waits for it. The log says `[HUI] scan extra roots=N embedded`; `NOT embedded` means an old helper.
A film's collection is named from the deepest root it is under, as an extra folder may hold a
`movies` folder.

**The scan helper is silent.** It posts a console notification only when it fails. The "Found N
movies" message after a scan is the app's own toast at the bottom (`Services::scan_found`).

**Where things are written.** App-writable storage is `/download0/hui`: covers,
wallpapers and descriptions in `covers/` (a description is `<id>.txt`; an empty
one is stored as `"\n"` so TMDB is not asked again, because `CoverStore::exists`
needs size > 0; likewise `<id>.rating` is `"0"` and `<id>.genres` is `"-"` when TMDB has
none, and `<id>.title` is `"\n"`), what a file contains in `info/<id>.info`, the extra scan folders in `scan-folders.txt`,
resume points beside them. Settings are read from
`/app0/mediathek.env`.

**The settings page.** The gear tab (`Tab::settings` in `mediathek.cpp`) is a `ui::ListView` of
categories on the left and a `ui::Form` with the chosen category's rows on the right, rebuilt
when the list's focus moves (`build_settings_form`). Right or Cross enters the form; Left or Back
returns, except that a choice row keeps left and right for itself. Switches change with Cross only.
It edits the kit's `Settings`: *UI sounds* is `ui_volume` (0 off, else the default), *Debug
info* is `show_fps` (the shell's overlay), *Fixed billboard* is `pin_billboard` (on by default; keeps
`hero_` open on every shelf, `animate()`; the settings file is format version 6, older ones still
load with the defaults; *App language* is `ui_language`, *Wallpaper on the details page* is
`details_wallpaper`, read by `draw_details_art`; *Shelves on Home* is `home_shelves`, read by
`rebuild`: Continue Watching always, then all, own (Recently Added, collections, All Movies), genres
or decades, with the All Movies shelf as the fallback when the choice has no shelf of its own);
*Force subtitles* is `force_subtitles` and *Subtitle language* `subtitle_language` (0 none,
1 German, 2 English), both read by `start_playback`: a film starts with no subtitles unless forced,
and Triangle steps through `media::subtitle_order`, the preferred language first.
A value is added within format version 5: an older build reading 3 clamps it to genres. The categories are Appearance, Playback, General, Library and About (`SettingsCategory`). A read-only *Version* row under About shows
the version and `build_time()` (`core/build_info.cpp`, `__DATE__`/`__TIME__`; `tools/build.sh` touches the file so every build
stamps it). A change sets `context_.settings_changed`;
the shell saves `/download0/hui/settings.bin` 0.75 s after the last one, and `main.cpp`
(`apply_ui_sounds`, also at start-up) mutes the `ui` and `sfx` mixer buses. The music bus
is deliberately left at its tuned gain; do not route the kit's volume curve through it.
*Reload all* calls `refetch_cover` for every film, as the details page does for one; its toast counts
the jobs with `Services::cover_jobs` ("Fetching 3/26 ...", `update_reload_progress`) and waits while a film plays.

**Memory.** FFmpeg needs far more than the kit's original 128 MiB heap; a 1080p
H.264 MKV ran it out ("failures=260" in the log) and showed as blocks and frame
jumps. `src/runtime/app_heap.c` tries 384 MiB down to 128 in 8 MiB steps (the author's console grants
256: the 384 request fails), keeps a 24 MiB margin above 256, and
`hui_heap_capacity()` lets the player scale its queues to what it got. The log
lines `[HUI] heap ...` and `[HUI] playing/stall` are the evidence to ask for.

**Seeking.** The demux thread seeks with `av_seek_frame`. A file with no index (an MKV without
Cues; FFmpeg's matroska demuxer cannot seek by byte either, tested) is read from the last known
point up to the target, so the wait grows with the jump and with a slow drive. A seek that took
over 0.3 s or read over 16 MiB logs `[HUI] seek to ... took ... and read ... MB (container, N seek
points)`: few seek points and many MB mean such a file. Remuxing it (`ffmpeg -i in.mkv -c copy
out.mkv`) writes the index.

**The hardware decoder.** Films beyond 1080p (`media::wants_hw_decoder`) go to `libSceVideodec2`
(`src/platform/ps5/hw_decoder.cpp`), because the CPU tops out near 31 fps on a 4K60 10-bit film; FFmpeg still
demuxes, decodes the sound and plays the rest. No public documentation exists: the structures and
the call order come from EVO Player and ps5-hardware-video-decoding-research. The SDK has no stub, so
`tools/build.sh` builds one from `tooling/native/stubs/videodec2_stub.c` and links it *before*
`--as-needed`. Every name in it must exist on the console, or the app does not start at all (no log,
CE-108255-1), so list only what is called. The decoder takes access units in start-code form
(`hevc_mp4toannexb`, rebuilt per seek, or the parameter sets are not repeated) and cleaned of what it
refuses (`media::clean_hevc_unit`); after a random access RASL pictures stay out (`media::HevcEntry`). A
seek makes a new decoder on the same memory, because `Reset` leaves some 4K streams unable to restart.
The pictures carry no time and come in display order, so each takes the earliest outstanding stamp
(`media::PendingStamps`). They are 4:2:0 in two planes in direct memory, 10-bit as low-aligned 16-bit words
(not P010). A converter thread copies them (`media::copy_planes`, 4K luma halved to 1080p, 10 bits moved to
the top of the word), and on the frame thread `gfx::VideoConverter` draws the planes into the player's
RGBA texture with a shader: matrix, range, and for HDR (PQ, HLG) VLC's BT.2390 tone mapping with 203 nits
as white, BT.2020 into BT.709. The kit's renderer then shows that texture as ever (no second rendering
path); the converter puts back the GL state it changes. The hook is `VideoSurface::update_yuv`; if it fails
once, the player goes back to `media::yuv_to_rgba` on the CPU (no tone mapping) for the app's life.
`HUI_SIM_HDR=1 tools/host-snapshots.sh ...` makes the preview's simulated player send a 10-bit PQ test
card through the same converter. The peak comes from the stream's content light level, else its mastering
display, else 1000 nits (`media::hdr_peak_nits`); `[HUI] hdr ...` logs it. Its working memory is
flexible memory (`kernel.flexibleMemorySize`, which is why the heap leaves room), its pictures direct
memory, so it does not touch the heap. The klog names each step *before* it enters the system's code
(`[HUI] hw decoder: ...`): a bad value there can kill the process without returning. `HW_DECODE=0` in
`mediathek.env` switches it off. Do not skip the 1 MiB stack on the video thread: the decoder overflows
the default one. Evidence to ask for: `[HUI] heap` (`of=` shows whether the bigger pool applied),
`[HUI] hw decoder`, and `(hardware)` with the times in `[HUI] playing/stall`.

**Playback.** Below that, software decode on up to 4 frame threads, an audio-clock-driven
`StreamRing`, a 4 MB AVIO read buffer. Pausing and resuming re-seeks. 4K on this path is not
expected to be smooth. HDR (PQ, HLG) leaves swscale out: `show_planes` copies FFmpeg's three planes
(`yuv420p`, `yuv420p10le`) into the hardware decoder's layout (`media::copy_planes` interleaves U and
V) and the same `gfx::VideoConverter` tone-maps it; SDR stays on swscale to RGBA, and so does HDR in
any other layout (logged once as `[HUI] hdr picture in ...`). Embedded subtitle tracks are not
shown; `.srt` files are.

**AV1 does not play, at any size.** FFmpeg's `av1` decoder is only a front for hardware decoding,
PacBrew's FFmpeg has no `libdav1d` or `libaom`, and no AV1 codec of `libSceVideodec2` is known (VLC's
PS5 port sends H.264, HEVC and VP9 to it). The way planned, as VLC's port does it: build dav1d 1.5.4
for the console (static, with its x86 assembly through nasm; `meson` is not installed on the author's
PC), feed it the demuxed packets on a decoding path of its own, and hand its pictures to the GPU
converter (its chroma comes in two planes, which `copy_planes` already interleaves for FFmpeg's HDR). 1080p should keep up;
whether 4K does depends on the CPU and is open. The owner deferred it after 0.7.0.

**Decoder memory.** Each frame thread holds frames of its own: 4K 10-bit HEVC needs about
175 MiB on one thread and 320 MiB on four (measured with FFmpeg on a PC), the 1080p
H.264 case a fraction of that. `media::decoder_threads` (`src/media/decode_budget.cpp`)
picks the threads that fit into what `hui_heap_stats` says is left, and the player
refuses a film that fits on none (`err_too_big`) rather than run the fixed heap out,
which aborts the app (no exceptions: a failed allocation is a crash). With one
frame thread left the decoder uses 4 slice threads (no extra frames; helps only WPP streams,
x265's default). The author's console grants a 336 MiB heap; one frame thread decodes their 4K
10-bit HEVC sample at ~30 ms/frame, which is why it now gets two where they fit. A decoder
slower than the film (the author's console decodes a 4K 59.94 fps 10-bit HEVC file at ~31 fps)
is held in step with the sound by `media::DecodePace` (`src/media/decode_pace.cpp`; it does nothing
until the first picture after a start or seek is out, because the way from the key frame to the target only looks like lag): on one
frame thread it leaves out non-reference frames when 0.25 s behind, and on any number it jumps
to a key frame near the sound when 0.8 s behind (`Session::keep_pace`). Several frame threads
keep the non-reference frames on purpose: they are the work that runs in parallel (measured on
a 4K 59.94 fps sample: 47 / 75 / 93 / 121 fps on 1 / 2 / 3 / 4 threads with everything, 32 fps
on any number without the B-frames). It works only because the
clock follows the sound in the `buffering` state too: a frozen clock sent the decoder after a
time the sound had left. Do not switch the loop filter off to save time: it smears into every
frame predicted from it. The estimate in `decode_budget.cpp` is fitted to the console's heap
peak (238 MB, 35 MB of it the app, for one 10-bit 4K thread), not to a PC. The log
line `[HUI] decoder WxH N-bit threads=... need=... room=...` is the evidence to ask for.

**TMDB.** A v3 key or a v4 token. One request per film brings the poster, the
wallpaper and the description (`overview`), and only for a film with no cover:
`missing_covers` skips every film that has a poster, its own or a fetched one
(fetched art is kept in `/download0/hui/covers` and filled back in at start-up),
so a rescan or restart asks TMDB about new films only; "Refresh cover and info"
is the way to fetch the rest. A film's genres are TMDB's list, up to four, in no
order of importance; the details page and the Home shelves use the first two
(`main_genres`). A film's title is TMDB's (`<id>.title`, filled in by `CoverStore::fill`, which
re-sorts nothing: `main.cpp` calls `Library::sort` after it, and the design finds the focused and
the playing film again by id); `Movie::file_title`, from the file name, is what TMDB is searched
with, what is shown until TMDB has answered or when it knows no title, and what search also
matches. Fetched pictures never replace
pictures the user put beside the film. The language follows the app unless
`TMDB_LANGUAGE` says otherwise. The app has its own DNS client and a CA bundle
in `assets/certs`; `DNS_SERVER` helps behind a router that blocks public DNS.

**Pitfalls that have already cost time**

- Shell heredocs mangle backslashes. Write source files with an editor tool.
- A local function named `tr` clashes with `hui::i18n::tr` through ADL; use
  `using i18n::tr;`.
- `Text` given a top coordinate instead of a baseline lands about one text
  height too high.
- A `static` local inside `draw()` is shared by both passes of a transition.
- A long string literal split across two lines inside an array looks like a
  missing comma to the compiler; keep each entry on one line.
- Item structs with `std::string` members cannot be brace-initialised partially
  under `-Wextra -Werror`; fill them field by field.
- Float loop counters fail static analysis; count with an `int`.
- `std::memcpy` with a null pointer is undefined even for zero bytes.
- A title uploaded for the first time is launchable only after the console has
  registered it (about ten seconds after its folder stops changing); the home
  screen assets are copied once at that moment.
- `make` output can be condensed by the shell wrapper on this machine. Re-run a
  command as `rtk proxy <cmd>` only if its result is empty or contradicts its exit
  code.

## Console work

Read [docs/CONSOLE_VALIDATION.md](docs/CONSOLE_VALIDATION.md) first. In short:

- A request to build or change something is **not** permission to launch on a
  console. **Ask the owner before each run**, say what it does and how long it
  takes. Uploading (FTP, `make deploy`) is a console action too.
- One run at a time. Never chain unattended runs. Never kill a title that is
  rendering; the app ends itself through the system.
- If the console stops answering, stop and report what you saw. Do not retry.
- Never touch console settings.
- The owner reads the klog (`nc <console> 3232`); ask for specific `[HUI]` lines
  rather than guessing.

## What "done" means

- The pictures look right, in English and in German where text changed, and you
  have looked at each one.
- `make test-unit` passes; `make` builds without warnings from your files;
  `make format-check` and `make lint` pass.
- Docs that mention what you changed are updated (`README.md`, `CHANGELOG.md`,
  `.env.example`, this file).
- You say plainly what you did not verify. Frame rate, sound, controller feel,
  FFmpeg playback speed and the network are *not verified* until someone has
  checked them on hardware; a PC picture never counts as a console result. The
  owner has checked language detection, subtitle and audio switching, Back/exit,
  menu sounds, controller feedback, stepping between films, genre shelves, search,
  the settings page (categories and settings panes, UI sounds, debug info, reload all with its progress message, shelves on Home) and internal and external drives on firmware 12.70. 4K HEVC films, HDR
  included, play smoothly on the hardware decoder with tone mapping (two checked, 0.7.0). HDR up to
  1080p with tone mapping, Force subtitles and Subtitle language, the regrouped Settings and the
  one-shelf Home and details page were checked in 0.8.0.
