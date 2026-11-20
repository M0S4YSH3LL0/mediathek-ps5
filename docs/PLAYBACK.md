# Playback and controls

## Formats

| | |
| --- | --- |
| **Files** | `.mkv` `.mp4` `.m4v` `.mov` `.avi` `.wmv` `.ts` `.m2ts` `.mpg` `.mpeg` `.webm` `.flv` `.vob` `.ogv` |
| **Video** | H.264, HEVC, AV1, VP9, VP8, MPEG-4 (Xvid, DivX), MPEG-2, MPEG-1, VC-1, WMV |
| **Audio** | AAC, AC-3, E-AC-3, TrueHD, DTS, MP2, MP3, Opus, Vorbis, FLAC, ALAC, WMA, PCM |
| **Subtitles** | Inside the film: SubRip, ASS/SSA (as plain text), mov_text, WebVTT, PGS, VobSub, DVB. Beside it: `.srt`. |
| **Pictures** | `.jpg` `.jpeg` `.png` |

H.264 (up to High) and HEVC (Main and Main10) films of any size up to 3840 x 2176 go to
the console's hardware decoder; AV1 is decoded on the CPU by dav1d, and FFmpeg decodes
everything else on the CPU, and takes over a film the hardware refuses or gives up on.

- **Plays smoothly on the hardware decoder:** H.264 and HEVC at every size: 4K HEVC, HDR
  included, and 1080p H.264 and 10-bit HEVC, smaller films and interlaced recordings.
- **Do not expect smooth playback:** 4K films the hardware decoder will not take (VP9,
  4:2:2, 12-bit, wider than 3840).
- **AV1** (dav1d, on six of the CPU's threads), HDR included: a 1080p HDR film plays
  smoothly on the console, seeking included; whether 4K keeps up is not checked yet (the
  console's hardware decoder is not known to take AV1).

A file with a codec the app cannot decode is still listed, but will not play. Audio
tracks it cannot decode are left out of the list.

## HDR

HDR films (HDR10 and HLG) reach a TV the console already sends HDR to as HDR10 (PQ,
BT.2020), the menus and controls drawn over them at their usual brightness; on an ordinary
screen, or with *HDR output* off, they are tone-mapped to SDR. Both work on every decoder,
as long as the software-decoded films are 8- or 10-bit 4:2:0, which is what HDR films are.
The console's own HDR setting decides whether the TV gets HDR at all.

## Sound

A 5.1 or 7.1 track plays on as many channels; the console mixes it down for two speakers.
*Audio output* Stereo in Settings makes the app mix it down instead.

## Controls

| | Browse | Details | Player |
| --- | --- | --- | --- |
| D-pad / stick | move | up/down: action<br>left/right: previous / next film, or season / episode on their rows | left/right: seek |
| Cross | details (Settings: open, change) | choose | pause / resume |
| Square | play or resume | | next audio track |
| Triangle | watched (Home)<br>search (Movies, Series) | | next subtitles, then off |
| R3 | sort (Movies, Series) | | |
| Options | scan again | | stream info |
| L1 / R1 | Home / Movies / Series / Settings | previous / next film | L1: from the beginning; twice in a series: previous episode<br>R1: seek +60 s; in a series: next episode |
| L2 / R2 | | | seek -60 s / +60 s |
| Circle | back (twice on Home to exit) | back | stop |

In the player, a tap seeks 10 s. Hold and the jump starts at 30 s and grows by 30 s
every second, up to 5 minutes. The player remembers where you stopped; 92% counts as
watched.

In search, every word you type must be in the title; accents and case do not matter.
Back clears the search first.
