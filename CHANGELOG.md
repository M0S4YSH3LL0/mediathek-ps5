# Changelog

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
