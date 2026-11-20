# What has been tested

Version 1.1.0 (`contentVersion` 01.000.110).

**Checked on the author's console, firmware 12.70:** the scan, covers and descriptions,
the details page, a large 1080p H.264 MKV with sound, seeking, audio and subtitle
switching, language detection, genre shelves, search, sorting, Settings, menu sounds
and controller feedback, with films on internal and external drives, 4K HEVC HDR
films on the hardware decoder with tone mapping, and (0.8.0) HDR films up to 1080p with tone
mapping, forced subtitles and the subtitle language, the regrouped Settings, and the simpler
Home and details page; and (0.9.0) series (the scan of the series folders, TMDB's
season and episode descriptions, choosing season and episode, the next episode in the
player), *Recently Added* going by the day a film was found, the media folders with
their choice of Automatic, Movies or Series, films shown with their title from TMDB, and the
red light bar; and (1.0.0) H.264 and HEVC of every size on the hardware decoder, the subtitles
inside a film and switching them mid-film and after a seek, the subtitle source, HDR output on
an HDR TV, 5.1 sound on the 8-channel output, the audio language and the Stereo output, and
a film's own forced subtitles shown by themselves, following the sound switched with Square;
and (1.1.0) AV1 through dav1d (a 1080p HDR film, seeking included), the larger details page,
the year from TMDB and L1 in the player. The loader version was not recorded. Reports from
other firmware are welcome.

**Not checked on a console:** 4K AV1 films, 4K H.264, HLG and Dolby Vision films, 7.1 sound,
a forced subtitle file (`Film.de.forced.srt`), and the fallback to the CPU when the graphics
chip cannot convert a picture.

**Checked on a PC:** the unit tests, the preview pictures, the scan helper against a
folder of sample files, a clean build and lint.
