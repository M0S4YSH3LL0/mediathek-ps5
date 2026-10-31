# Instructions for AI agents

This file is for coding agents (and people in a hurry). It says what this
repository is, where to look, how to work in it, and what "done" means here.

## What this is

**Mediathek**: a Netflix-style movie library and player for the PS5, a native
C++20 app drawn with OpenGL 4.6 (title id `PPSA66120`, version in
`sce_sys/param.json`). It finds films in `movies` folders on the internal drive,
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
                               runtime and the cache text of what a file contains;
                               info_service: reads files in the background):
                               library (scan index -> Movie list, a film's subtitle
                               files, genre shelves, search matching), title (file
                               name -> title, year, quality; search_fold), progress (resume
                               points, watched), subtitles (.srt), tmdb + cover_service
                               (poster, wallpaper, description), poster_cache,
                               player.hpp (the interface), simulated_player (PC preview)
src/core/i18n*               texts: i18n_texts.inc is THE table (English, German)
src/platform/ps5/            ffmpeg_player, media_probe (stream list of a file, no
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
export LLVM_VERSION=19 HOST_CXX=clang++-19 HOST_CC=clang-19 USE_CCACHE=0   # this machine's toolchain

tools/host-snapshots.sh build/snapshots mediathek    # PC preview: PNG frames of every screen
HUI_LANG=de tools/host-snapshots.sh build/snapshots-de mediathek   # the same in German
make test-unit                                       # GoogleTest, sanitizers on
make format-check                                    # clang-format (make format fixes)
make                                                 # the PS5 app folder in dist/PPSA66120
make lint                                            # needs a git index, see below
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

`make lint` runs `git ls-files`, and this folder is not a git repository. Give it
a throwaway index (takes more than two minutes; use a long timeout):

```bash
rm -rf /tmp/lintgit
export GIT_DIR=/tmp/lintgit GIT_WORK_TREE=$PWD
git init -q && git add -A && make lint
```

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
  `mediathek.env` overrides it. The log line `[HUI] language console=... ui=...`
  says what happened. Parameter id `2` is the date format, not the language.
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
  copies only `TMDB_API_KEY`, `TMDB_LANGUAGE`, `DNS_SERVER` and `UI_LANGUAGE`, and
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

- This folder is not a git repository. Do not `git init`, commit, tag or push
  unless the owner asks. When asked: small commits, one-line imperative subject.
- A release tag must equal `sce_sys/param.json` `contentVersion` exactly
  (`01.000.040` is the app's 0.4.0); the app shows the short form (`v0.4.0` in
  `mediathek.cpp`). Change both, `README.md` (Status) and `CHANGELOG.md` together.
- Never commit build output, console evidence, addresses of machines, personal
  paths or credentials.

## Things that are not obvious

**A sandboxed title cannot list a folder.** The app sends `mt-scan.elf` to an ELF
loader on `127.0.0.1:9021`; it walks the `movies` folders, copies small pictures
and subtitles next to the app and writes `mediathek-index.txt`, which the app
reads as `/app0/mediathek-index.txt`. Without a loader the app starts and finds
nothing. The install folder `/data/homebrew/PPSA66120` is `/app0` inside the app.

**Where things are written.** App-writable storage is `/download0/hui`: covers,
wallpapers and descriptions in `covers/` (a description is `<id>.txt`; an empty
one is stored as `"\n"` so TMDB is not asked again, because `CoverStore::exists`
needs size > 0; likewise `<id>.rating` is `"0"` and `<id>.genres` is `"-"` when TMDB has
none), what a file contains in `info/<id>.info`, resume points beside them. Settings are read from
`/app0/mediathek.env`.

**Memory.** FFmpeg needs far more than the kit's original 128 MiB heap; a 1080p
H.264 MKV ran it out ("failures=260" in the log) and showed as blocks and frame
jumps. `src/runtime/app_heap.c` tries 384, 256, 192 then 128 MiB, and
`hui_heap_capacity()` lets the player scale its queues to what it got. The log
lines `[HUI] heap ...` and `[HUI] playing/stall` are the evidence to ask for.

**Playback.** Software decode with 3 threads, an audio-clock-driven `StreamRing`,
a 4 MB AVIO read buffer. Pausing and resuming re-seeks. 4K, 10-bit HEVC and HDR
are not expected to be smooth. Embedded subtitle tracks are not shown; `.srt`
files are.

**TMDB.** A v3 key or a v4 token. One request per film brings the poster, the
wallpaper and the description (`overview`), and only for a film with no cover:
`missing_covers` skips every film that has a poster, its own or a fetched one
(fetched art is kept in `/download0/hui/covers` and filled back in at start-up),
so a rescan or restart asks TMDB about new films only; "Refresh cover and info"
is the way to fetch the rest. A film's genres are TMDB's list, up to four, in no
order of importance; the details page and the Home shelves use the first two
(`main_genres`). Fetched pictures never replace
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
  menu sounds, controller feedback, stepping between films, genre shelves, search
  and internal and external drives on firmware 12.70; 4K and HEVC playback are
  still unchecked.
