# Changelog

## Unreleased

- Films are also found below a `mediathek/movies` folder (or `mediathek/Movies`) on the
  internal drive, USB drives and extended storage, beside the `movies` folder as before. Its
  subfolders become collections in the same way.

## 0.6.4 (01.000.064)

- *Shelves on Home* gains a fourth option, *Decades only*, and *Genres* is now *Genres only*: it
  shows the genre shelves without the decades. Decades appear under *All* and *Decades only*.
- Continue Watching now stays on Home under every option; 0.6.3 hid it under *Genres*. If *Genres
  only* or *Decades only* has no shelf of its own (no film has a genre yet, or fewer than 12
  films), All Movies is added so the films are still there.
- *Reload all covers and infos* now shows its progress in a message at the bottom: "Fetching 3/26
  covers and infos...", then "Finished fetching covers and infos". The count comes from the
  download queue, so it includes covers still on their way from a scan. Other messages show first,
  and while a film plays the message waits until the player closes. A reload that TMDB stops shows
  the problem instead of "Finished".

## 0.6.3 (01.000.063)

- The Settings page is rebuilt in two panes. On the left is a list of categories (Sound,
  Appearance, Language, Library, Developer); on the right, the settings of the one you are on.
  Right or Cross moves into the settings; Left or Circle moves back to the list, and Circle
  there goes to Home. Both sides scroll when they are too long.
- Switches on the Settings page change with Cross only. Left and right no longer flip them,
  because Left is now the way back to the list. A row with several options, such as *App
  language*, still uses left and right, so Circle is the way back from there.
- A new setting, *Shelves on Home* (Settings, under Appearance; All by default). *Own collections
  only* shows Continue Watching, Recently Added, your folder collections and All Movies; *Genres*
  shows the genre and decade shelves. Home changes at once. If *Genres* would leave Home empty
  (no film has a genre yet and the library is small), it shows All Movies instead. The settings
  file gains one byte (format version 5); earlier files load as All.
- The version and build time moved from the bottom left of the Settings page to a read-only
  *Version* row under Developer.

## 0.6.2 (01.000.062)

- While the film is paused, the Cross hint in the player reads "Resume" ("Fortsetzen") instead of
  "Pause".
- A new setting, *Wallpaper on the details page* (Settings, under Appearance; on by default).
  Off, the details page shows only the film's colour with its glow and fades, as it does for a
  film without a wallpaper. The Home heading of the settings is now called Appearance, and *Reload all covers and infos*
  now sits above *Debug info*. The
  settings file gains one byte (format version 4); earlier files load with the wallpaper on.
- A new setting, *App language* (Settings, under Language): Automatic (the console's language,
  or UI_LANGUAGE in mediathek.env), English or Deutsch. It changes every text at once, shelves
  and Settings page included. A choice made there wins over UI_LANGUAGE. Descriptions fetched
  from TMDB follow the new language after the next start. The settings file gains one byte
  (format version 3); earlier files load as Automatic.

## 0.6.1 (01.000.061)

- *Fixed billboard* is on by default. A settings file saved by 0.6.0 keeps whatever it holds.
- The Home billboard has only the Play (or Resume) button: the Details button next to it is gone
  (Cross on a film still opens the details page, and the hint at the bottom says so).
- The Settings page shows the version and the date and time of the build at the bottom left, to
  tell which build the console runs.
- "Found N movies" after a scan is now the app's own small message at the bottom, after
  "Looking for movies...", instead of a console notification. The scan helper only posts a
  notification when the scan fails.

## 0.6.0 (01.000.060)

- Subtitle files get a language the same way films get their sound languages. A name that says
  it ("Film.de.srt", "2_English.srt") still wins, and now so does a short name that starts with
  the language ("English.srt", "Deutsch.forced.srt", "ger.srt") as found in "Subs" folders. A
  file whose name says nothing ("Film (2011).srt") was listed as "Unknown language"; its text is
  now read once, when the film's page opens or the film starts, and the language is guessed from
  its common words (English, German, French, Spanish, Italian, Portuguese, Dutch) when the
  result is clear. The details page and the Triangle toast use it.
- Holding left or right in the player now seeks faster the longer you hold: a tap is 10 s, a
  hold starts at 30 s and adds 30 s per second held, up to 5 minutes per step.
- A seek that takes long (over 0.3 s) or reads a lot of the file (over 16 MB) is now named in
  the klog with its container and how many seek points the file has. Films whose file has no
  index (an MKV without Cues, say) make FFmpeg read through the file up to the point you jump
  to, so the farther the jump the longer the wait; the line tells those from a slow drive.
- A film whose file does not say what language its sound is in (no tag, or "unknown"), or that
  could not be read, is now listed in the language its file name names: "Film (2011) German
  DL.mkv" shows German instead of "Unknown language" (or "Not available"). The file's own tags
  still win, only the words after the title count ("The English Patient" is not English), and a
  language named as subtitles ("Eng.Sub") is none. It applies to the details page at once,
  without a refresh. A rescan of the folders does not read the files again; "Refresh cover and
  info" and "Reload all covers and infos" do.
- A new setting, *Fixed billboard* (Settings, under Home; off by default): the Home billboard
  with the art, title, Play and Details stays in place while you scroll down through the
  shelves, and shows whichever film is focused. Off, it collapses once you leave the first
  shelf, as before. The settings file gains one byte (format version 2); a file from an
  earlier version still loads, with the setting off.
- The toolchain is LLVM 19 (Makefile, scripts, docs and the GitHub workflow). CI had used
  clang-format 18, which lays out a few constructs differently from 19, so the Build workflow
  failed at "Lint source and metadata" for every release since 0.3.0 while `make lint` passed
  on a machine with 19.

## 0.5.0 (01.000.050)

- A Settings page: the gear at the top right, a third tab after Home and Movies (L1/R1).
  *UI sounds* (on by default) switch the menu sounds off, *Debug info* (off by default)
  shows the frame rate and drawing statistics, and *Reload all covers and infos* asks,
  then fetches cover, wallpaper and description of every film again and re-reads every
  file; pictures beside a film are left alone. Changes apply at once and are saved by
  themselves. The saved choice now also applies at start-up.
- `tools/prepare-assets.sh` works on Debian: without PowerShell it converts the icon and
  backgrounds with Python (Pillow, NumPy), see `docs/platform/PRESENTATION_ASSETS.md`.
- New icon, and a new selection and launch background (`sce_sys/background-source.png`,
  which is also the README's header picture).

## 0.4.0 (01.000.040)

- On the details page L1/R1 and left/right step to the previous or next film of the
  shelf or grid the page was opened from; the page fades the new film in. At either
  end the pad refuses. Back returns to the film that was last shown.
- The red line naming the shelf is back above the billboard title for every shelf,
  also *Your Movies* and *All Movies*.
- Home gets a shelf per TMDB genre, between the collections and the decades (which stay).
  A film is on the shelves of the two genres its details page names, and only those, so
  every genre named there has a shelf, even with one film; the fullest shelves come
  first. Genres come with the TMDB answer, so a library without a key or without fetched
  info has none.
- The Movies page has a search: Triangle opens an on-screen keyboard (Triangle is a
  space, Square deletes, Options is Done). Every word must be in the title,
  ignoring case and accents; Done with an empty line clears it.
  Back clears an active search before it leaves the page. Sorting moved to R3.
- A film can have several subtitle files (up to eight; English first, then in the
  order found). In the player Triangle steps through them and then off, and says
  which one is on ("Subtitles: Deutsch (2/3)"); with one file it is the old on/off
  switch. The details page lists the languages of all of them. Square still steps
  through the audio tracks.
- The home screen gets a proper selection and launch background (`sce_sys/pic0.dds`,
  `pic1.dds`: the same picture, 3840x2160 BC7), made from `sce_sys/background-source.png`.
- Moving between shelves plays the same sound on every row; it used to drop in pitch
  the further down you went.

## 0.3.0 (01.000.030)

- The details page shows more: genres next to the collection above the title, chips
  for the year, TMDB's user score (out of 10), length, video format (codec,
  resolution, container) and sound format, and rows for the audio languages (the
  tracks the app can decode) and the subtitle language (the `.srt` beside the film). The
  length, formats and languages are read from the file with FFmpeg when the page
  opens and kept in `/download0/hui/info`; the score and genres come with the
  TMDB search answer. Films that already had a cover are asked once for their score
  and genres. The size moved to the file line.

- Covers, wallpapers and descriptions are fetched only for films that have no
  cover. A rescan or restart no longer asks TMDB about films that already have
  one (their own or a fetched one), so films TMDB has no wallpaper or description
  for are not searched for again. "Refresh cover and info" still fetches
  everything for one film.

## 0.2.0 (01.000.020)

- The details sheet is now a full-screen page: the film's wallpaper behind it,
  the cover on the left, title, year, quality and size in the middle with a
  description, and the actions on the right. The description is fetched from
  TMDB with the cover and kept with it (one request per film, also for films
  that already have their pictures). "Fetch cover again" became "Refresh cover
  and info".
- The app speaks English and German. It follows the console's language setting
  (German on a German console, English otherwise); `UI_LANGUAGE=en` or `de` in
  `mediathek.env` overrides that. Descriptions from TMDB come in the same
  language unless `TMDB_LANGUAGE` is set. The fonts now cover Latin-1 and
  Latin-Extended-A, Greek and Cyrillic, so umlauts and accented film titles
  draw.
- On the details page the file name and status sit lower, and a long
  description now ends with an ellipsis above them instead of running into them.
- The billboard no longer shows a red "Your Movies" / "All Movies" line above the title.
- Fixed: the console language was read from the wrong system parameter (the
  date format), so a German console showed English.
- Playback: a bigger memory budget (up to 384 MiB, scaled to what the console
  grants) and larger read buffers fix glitches in big 1080p files.

Release tags are the exact `contentVersion` of `sce_sys/param.json`; the version
shown in the app is the short form.

## 0.1.0 (01.000.010)

The first release.

- Home with a billboard (wallpaper or poster) and shelves: Continue Watching,
  Recently Added, collections, decades, All Movies. A Movies grid with sorting.
  A details sheet. A player with seeking, pause, audio tracks and `.srt`
  subtitles, resume points and a watched mark.
- Finds films below `movies` folders on the internal drive, USB drives and
  extended storage, up to six levels deep, with subtitles beside the film or in
  a `subs` folder. A helper payload sent to the ELF loader on `127.0.0.1:9021`
  does the listing the sandbox does not allow.
- Fetches posters and wallpapers from TMDB for films without pictures, with
  your own free key in `mediathek.env`, and a "Fetch cover again" action.
- Circle goes back; on the first film of Home, pressing it twice exits.
- Playback of large files: the film is read through a 4 MB buffer instead of
  FFmpeg's 32 KB, and the app takes the biggest heap the console grants (up to
  384 MiB, at least 128) with packet queues sized to it. The 128 MiB heap ran
  out during a busy 1080p H.264 film and caused blocks, smears and jumps.
  FFmpeg's "fast" decoding flag is off for the same kind of glitch.
- Pausing silences the sound; resuming starts again from the same spot.
- The log (klog) gets one `[HUI] playing …` line every ten seconds while a film
  plays, and a `[HUI] stall …` line when the picture runs dry.
