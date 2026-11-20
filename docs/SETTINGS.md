# Settings, covers and language

## Covers and descriptions

1. Get a free key at [themoviedb.org](https://www.themoviedb.org/settings/api).
2. Create `/data/homebrew/PPSA66120/mediathek.env` on the console:

   ```
   TMDB_API_KEY=your-key-here
   ```

3. Start the app. After each scan it looks up films and series with no cover, one at a
   time in the background. A series brings the description of each season and the name
   and description of each episode, and a season added later is fetched on its own.

Without a key, films get generated covers.

Films are shown with TMDB's title, with the colons, commas and dashes a file name drops:
`Blue Harbor Part Two (2014).mkv` becomes *Blue Harbor: Part Two*. Until TMDB has
answered, or when it does not know the film, the title comes from the file name, and
search finds a film by either title.

A film that has a cover, or that TMDB does not know, is not looked up again. To fetch
again, use **Refresh cover and info** on the details page, or **Reload all covers and
infos** in Settings.

Keep your key private. When you build from source, `make` writes this file from your
`.env` and never puts it in the `.zip`.

*This product uses the TMDB API but is not endorsed or certified by TMDB.*

## mediathek.env

The same file takes a few optional settings:

| Setting | What it does |
| --- | --- |
| `TMDB_LANGUAGE=en-US` | Language of descriptions and titles. Follows the app if left out. |
| `UI_LANGUAGE=de` | `en`, `de` or `auto`, the default. See [Language](#language). |
| `DNS_SERVER=192.168.0.1` | Your router's address, if your network blocks public DNS. |

## The settings page

The gear at the top right opens Settings: categories on the left, their settings on the
right. Right or Cross moves into the settings, Left or Circle moves back, and Cross
changes a setting. On a row with several options, left and right pick the option, so use
Circle to go back. Changes apply at once and save themselves.

| Category | Setting | What it does |
| --- | --- | --- |
| Appearance | Fixed billboard | Keeps the Play button on screen while you scroll the shelves. |
| Appearance | Shelves on Home | *Continue Watching* always, then all shelves, your own collections only (*Recently Added*, collections, *All Movies*, *All Series*), genres only or decades only. |
| Appearance | Wallpaper on the details page | Shows the film's wallpaper behind its details. |
| Playback: Subtitles | Force subtitles | A film with subtitles starts with them on. Off by default. |
| Playback: Subtitles | Subtitle language | None, German or English: the subtitles chosen first, when forced on or switched on with Triangle. |
| Playback: Subtitles | Subtitle source | Automatic (the film's own tracks and the `.srt` files beside it, the film's own first), Embedded only or SRT only. |
| Playback: Audio | Audio language | Automatic, German or English: the sound track a film starts with. Automatic plays the track the file chooses; German or English plays that language, else English, else as Automatic. Within a language the main track comes before a commentary. From the next film. |
| Playback: Audio | Audio output | Automatic: a 5.1 or 7.1 track plays on as many channels (the console mixes it down for two speakers). Stereo: the app mixes it down. From the next film. |
| Playback: Video | HDR output | Automatic: an HDR film goes to an HDR TV as HDR10 while it plays. Off: always tone-mapped to SDR. |
| General | App language | Automatic, English or Deutsch. |
| General | UI sounds | Menu sounds on or off. |
| Library | Reload all covers and infos | Fetches everything from TMDB again, with a message that counts the progress. |
| Library | Media folders | Folders searched besides `movies`, `tvshows`, `series` and those below `mediathek`: add a path or a name, choose Automatic, Movies or Series for each with left and right, or choose one to remove it. Each change scans again. See [Your library](LIBRARY.md#other-folders). |
| About | Version | The version and when this build was made. |
| About | Author | Who made the app. |
| About | Debug info | Frame rate in the corner. |

## Language

The app speaks English and German. *Automatic* follows the console, German on a German
console and English otherwise. Set `UI_LANGUAGE` in `mediathek.env` to change what
*Automatic* picks; *App language* in Settings wins over both.
