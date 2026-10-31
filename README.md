<p align="center"><img src="sce_sys/background-source.png" alt="Mediathek: a dark cinema with red seats and a film projector" width="100%"></p>

<div align="center">

<h1>Mediathek</h1>

**A Netflix-style movie library for your PS5.**
Put your films in a `movies` folder; Mediathek finds them, dresses them up with
posters and wallpapers, and plays them. English and German.

[![Version 0.6.2](https://img.shields.io/badge/version-0.6.2-blue)](CHANGELOG.md)
[![Platform: PS5](https://img.shields.io/badge/platform-PS5-informational)](#what-you-need)
[![Tested on: PS5 firmware 12.70](https://img.shields.io/badge/tested%20on-FW%2012.70-success)](#status)
[![Languages: English, German](https://img.shields.io/badge/languages-EN%20%7C%20DE-green)](#language)
[![Licence: GPL-3.0-or-later](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue)](LICENSE)
[![Downloads](https://img.shields.io/github/downloads/M0S4YSH3LL0/mediathek-ps5/total?style=flat&color=8ea2ff&label=Downloads)](https://github.com/M0S4YSH3LL0/mediathek-ps5/releases)

**Get it on [ProsperoStore](https://prosperostore.net/app/PPSA66120/)** (the recommended way to install).

[Install](#install) · [Your files](#your-files) · [Covers](#covers-and-descriptions) ·
[Controls](#controls) · [Good to know](#good-to-know) · [FAQ](#faq) · [Build](#build-from-source)

</div>

![Home: the billboard and the shelves](docs/screenshots/home.jpg)

| | |
| --- | --- |
| ![All movies](docs/screenshots/movies.jpg) | ![Details](docs/screenshots/details.jpg) |
| ![Genre shelves on Home](docs/screenshots/genres.jpg) | ![Search on the Movies page](docs/screenshots/search.jpg) |
| ![Settings](docs/screenshots/settings.jpg) | ![Home, in German](docs/screenshots/home-de.jpg) |

*The pictures come from the PC preview with invented films. On a console the
covers and wallpapers are the real ones, fetched by the app.*

## Why Mediathek

- **Your films, found for you.** Point it at a `movies` folder on the internal
  drive, a USB stick or extended storage. `Blue Harbor (2011).mkv` and
  `blue.harbor.2011.2160p.BluRay.x265-GRP.mkv` both work.
- **It looks like a streaming app.** A billboard for the focused film, shelves for
  *Continue Watching*, *Recently Added*, collections, genres and decades, and a
  grid with search and sorting.
- **Posters without the chore.** With a free TMDB key it fetches posters,
  wallpapers, descriptions, scores and genres. Without one you get generated
  covers. Pictures you put beside a film always win.
- **A proper details page.** Length, video and sound format, the languages of the
  audio and the subtitles, where you stopped, Play / Resume / Start over.
- **It remembers.** Resume where you left off; 92% counts as watched.
- **A real player.** Seeking that speeds up while you hold, audio track and `.srt`
  subtitle switching, a stream info panel.

## What you need

- A PS5 that runs homebrew, with a loader that starts apps from `/data/homebrew`
  and an **ELF loader listening on `127.0.0.1:9021`** (etaHEN and compatible
  loaders do). Without it the app starts but finds no films, because a sandboxed
  title cannot list folders on its own (see [how it finds your files](#how-it-finds-your-files)).
- Films in a folder called `movies` (or `Movies`) on any of your drives.
- Optional, for covers: a free [TMDB](https://www.themoviedb.org/settings/api) key
  and a network connection.

This repository does not set up a console. Follow your loader's own documentation
for that.

## Install

### Recommended: ProsperoStore

Mediathek is listed on [ProsperoStore](https://prosperostore.net/app/PPSA66120/)
([homebrew.page](https://homebrew.page)), a community catalog of native PS5 homebrew
apps. Install and update it from there with the
[ProsperoStore client](https://github.com/darkxex/ProsperoStore/releases) on your
console. The client downloads the app from this repository's releases, so you get the
same files as below without copying anything by hand. Use the manual steps only if you
prefer them, or if you build the app yourself.

### Manual

1. Download `Mediathek-PPSA66120-<version>.zip` from the [Releases](../../releases)
   page (check it against the `.sha256` next to it) and unpack it. You get a folder
   called `PPSA66120`.
2. Copy that folder to `/data/homebrew/` on the console (FTP, or whatever your
   loader offers), so that `/data/homebrew/PPSA66120/eboot.bin` exists.
3. Wait about ten seconds. The console registers a new title a moment after its
   folder stops changing, then Mediathek shows up on the home screen.
4. Start it. The first start scans for films, which takes a moment; after that the
   library appears at once. Press **Options** to scan again.

To build it yourself, see [Build from source](#build-from-source).

**Updating:** with ProsperoStore, update from the client. Manually, copy the new folder over the old one. If the home screen shows an old
icon or an app that will not start, delete the title first
(`/data/homebrew/PPSA66120`, plus the console's copies under `/user/app/PPSA66120`
and `/user/appmeta/PPSA66120`) and copy it again.

## Your files

Put films anywhere below a `movies` folder, up to six levels deep:

```
/data/movies/Blue Harbor (2011)/Blue Harbor (2011).mkv
/mnt/usb0/movies/Space Sagas/Orbital Dawn (2010).mkv
```

The folders searched are `/data/movies`, `/movies`, `/mnt/usb0-7/movies` and
`/mnt/ext0-1/movies`.

- **Collections:** a subfolder with two or more films becomes a shelf of its own.
- **Your own pictures:** `<film>-poster.jpg` and `<film>-fanart.jpg` in the film's
  folder. A folder holding a single film may use `poster.jpg`, `folder.jpg`,
  `cover.jpg` and `fanart.jpg`, `backdrop.jpg` instead.
- **Subtitles:** `.srt` files with the film's name, beside it or in a `subs` folder
  next to it (a film alone in its folder owns everything in `subs`). English comes
  first, then the others in the order found (up to eight); the first one shows when
  the film starts. The language comes from the file name (`Film.de.srt`,
  `English.srt`) or, if the name says none, from the words inside.
- **No picture?** The film gets a generated cover.

## Covers and descriptions

Mediathek can fetch the poster, a wide wallpaper, the description, TMDB's user score
and the genres from [The Movie Database](https://www.themoviedb.org/) by itself.

1. Get a free key for personal use at themoviedb.org/settings/api.
2. Create a text file `mediathek.env` in `/data/homebrew/PPSA66120/` on the console:

   ```
   TMDB_API_KEY=your-key-here
   ```

3. Start the app. After every scan it asks for the films that have no cover, one at
   a time in the background, and each new picture appears within a couple of seconds.

That is all. A few extras, all optional, go into the same file:

| Setting | What it does |
| --- | --- |
| `TMDB_LANGUAGE=en-US` | Language of descriptions and fetched titles. Follows the app (`en-US` or `de-DE`) when left out. |
| `UI_LANGUAGE=de` | `en`, `de` or `auto` (ask the console, the default). See [Language](#language). |
| `DNS_SERVER=192.168.0.1` | Your router's address, if your network blocks public DNS. |

Keep your key to yourself and out of anything you share. When you build from source,
`make` writes the file for you from the project's `.env`, never into the `.zip`.

A film that already has a cover, yours or fetched, is not asked about again, and
neither are films TMDB does not know. To fetch again, use **Refresh cover and info**
on a film's details page, or **Reload all covers and infos** in Settings.

*This product uses the TMDB API but is not endorsed or certified by TMDB.*

## Controls

| | Browse | Details | Player |
| --- | --- | --- | --- |
| D-pad / stick | move | up/down: action; left/right: previous / next film | left/right: seek |
| Cross | details (Settings: change) | choose | pause / resume |
| Square | play (resumes) | | next audio track |
| Triangle | watched (Home) / search (Movies) | | next subtitles, then off |
| R3 (press the right stick) | sort (Movies) | | |
| Options | scan again | | stream info |
| L1 / R1 | Home / Movies / Settings | previous / next film | seek -60 s / +60 s |
| Circle | back; on Home press twice to exit | back | stop |

In the player, a tap seeks 10 s. Hold left or right and the jump grows from 30 s by
another 30 s every second, up to 5 minutes.

## Language

The app speaks English or German. It follows the console's language (German on a
German console, English otherwise). To choose yourself, open **Settings → App
language** (Automatic, English or Deutsch; the texts change at once), or set
`UI_LANGUAGE` in `mediathek.env`, which is what *Automatic* then means. Another
language is one more column in `src/core/i18n_texts.inc`.

## Good to know

- **Settings** (the gear at the top right) has *UI sounds*, *Fixed billboard* (keeps
  the Play button on screen while you scroll the shelves), *Wallpaper on the details
  page*, *App language*, *Reload all covers and infos* and *Debug info* (frame rate
  in the corner). Changes apply at once and are saved by themselves. The bottom left
  shows the version and when this build was made.
- **Search** (Triangle on Movies): every word you type must be in the title; accents
  and case do not matter. Back clears the search first.
- **Details** shows the file's own facts too: codec, resolution, container, sound
  format. If a file does not say what language its sound is in, the language in its
  name stands in: `Film (2011) German DL.mkv` is listed as German.
- **Stepping:** on the details page, L1/R1 or left/right jump to the previous or next
  film of the shelf (or grid) you came from.
- **What the player can do:** it lists only what it can use, which means the audio
  tracks it can decode and the `.srt` beside the film. Subtitle tracks inside the
  file are not shown.
- **Video:** films are decoded in software on the CPU. A large 1080p H.264 MKV plays
  smoothly. 4K, HEVC (especially 10-bit) and HDR are **not** expected to be smooth,
  and HDR is not tone-mapped. Pausing and resuming re-seeks, so resuming takes a
  moment on a slow drive.

## FAQ

**The app starts but shows no films.**
The ELF loader on `127.0.0.1:9021` is not running, or there is no folder called
`movies` on a drive it searches. Start the loader, then press **Options** to scan again.

**Nothing happens after I copy the folder.**
Wait about ten seconds after the copy ends. If the title still does not show or an
old icon stays, delete the title and copy it again (see [Install](#install)).

**There are no covers.**
Check that `mediathek.env` is in `/data/homebrew/PPSA66120/`, that the key is right
and that the console is online. If your network blocks public DNS, add `DNS_SERVER`.

**Jumping far ahead in a film takes a long time.**
Some MKV files carry no seek index, so the player has to read through the file to
reach the spot. Remuxing the file writes the index:
`ffmpeg -i in.mkv -c copy out.mkv`.

**Does it have 4K or HDR?**
Not smoothly. See *Video* above.

**Does it come with films?**
No. Use your own legally obtained copies.

## How it finds your files

A sandboxed PS5 title can open a file it knows the path of, but cannot list a folder.
So Mediathek sends a small helper payload (`helper/mt-scan.c`, built into the app as
`assets/helper/mt-scan.elf`) to the ELF loader on `127.0.0.1:9021`. The helper walks
the folders above, copies the small pictures and subtitles into the install folder
and writes `mediathek-index.txt`, which the app reads as `/app0/mediathek-index.txt`.
The last index is read at start-up, which is why the library appears at once.

Fetched covers, descriptions and file facts are kept in the app's save area,
`/download0/hui`. A rescan of the folders does not discard them.

## Build from source

```bash
make                  # the PS5 app folder in dist/PPSA66120 (needs the toolchain make fetches)
make test-unit        # GoogleTest, sanitizers on
make format-check
make lint             # needs a git index; AGENTS.md says how to give it one

python3 tools/make-sample-movies.py                  # the invented films the preview shows
tools/host-snapshots.sh build/snapshots mediathek    # PC preview: PNG pictures of every screen
HUI_LANG=de tools/host-snapshots.sh build/snapshots-de mediathek   # ... in German
```

Copy `.env.example` to `.env` for your console's address (`make deploy`) and, if you
like, your TMDB key. The PC preview renders the same UI code with Mesa; it plays no
video (a simulated player draws a moving test picture) and makes no sound.
`python3 tools/fetch-posters.py /path/to/movies` fetches posters on a PC
(`--dry-run` lists what is missing).

Mediathek is a native C++20 app drawn with OpenGL 4.6, built on the
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) kit.
[AGENTS.md](AGENTS.md) and [CONTRIBUTING.md](CONTRIBUTING.md) describe the code and
the rules for changes. A release tag is the exact `contentVersion` of
`sce_sys/param.json`.

## Status

Version 0.6.2 (`contentVersion` 01.000.062).

**Checked on the author's console (firmware 12.70):** the scan, covers and
descriptions, the details page, playback of a large 1080p H.264 MKV with sound,
seeking, audio and subtitle switching, language detection, genre shelves, search,
sorting, the Settings page, menu sounds and controller feedback, with films on the
internal drive and on an external drive. The loader version was not recorded;
reports for other firmware versions are welcome.

**Not checked on a console yet:** 4K and HEVC playback. The unit tests, the PC
preview pictures, the scan helper against a folder of sample files and a clean
build and lint run on a PC.

## Credits

Mediathek stands on other people's work:

- [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) by
  BlackBearReloaded: the renderer, motion, themes, mixer and build this app is
  made from, with the sound effects from
  [ProsperoPuzzles](https://github.com/blackbearreloaded/ProsperoPuzzles).
- [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) and
  [Mesa](https://mesa3d.org/): OpenGL 4.6 on the PS5.
- The [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) and
  [PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate):
  the toolchain and native foundation.
- [FFmpeg](https://ffmpeg.org/), in PacBrew's build: it decodes the films.
- [The Movie Database](https://www.themoviedb.org/): posters, wallpapers and
  descriptions.
- The PS5 homebrew community and the authors of the loaders (etaHEN and
  compatible) that make running unsigned apps and the scan helper possible.
- [Inter](https://github.com/rsms/inter),
  [Montserrat](https://github.com/JulietaUla/Montserrat),
  [DejaVu](https://dejavu-fonts.github.io/),
  [Press Start 2P](https://fonts.google.com/specimen/Press+Start+2P),
  [Patrick Hand](https://fonts.google.com/specimen/Patrick+Hand) and
  [stb](https://github.com/nothings/stb).

Every third-party component, with its licence, is listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

Mediathek plays files you provide; it contains no films, and no film data ships with
it. It is not affiliated with Sony, TMDB or anyone whose film you play. All films and
brands in the screenshots are invented.

## Licence

GPL-3.0-or-later (see `LICENSE`). Third-party components, including FFmpeg and the
libraries linked with it, TMDB's API and the Mozilla CA bundle, are listed in
`THIRD_PARTY_NOTICES.md`.
