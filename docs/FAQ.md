# FAQ

**The app starts but shows no films.**
The ELF loader on `127.0.0.1:9021` is not running, or no searched drive has a `movies`,
`tvshows` or `series` folder (or one of those below `mediathek`), nor one you added in
Settings. Start the loader and press **Options**. See [Your library](LIBRARY.md).

**Nothing appears after I copy the folder.**
Wait ten seconds after the copy ends. If it still does not show, delete the title and
copy it again (see [Install](../README.md#install)).

**An update shows an old icon or will not start.**
Delete `/data/homebrew/PPSA66120`, `/user/app/PPSA66120` and `/user/appmeta/PPSA66120`,
and copy the folder again.

**There are no covers.**
Check that `mediathek.env` is in `/data/homebrew/PPSA66120/`, that the key is right
and that the console is online. If your network blocks public DNS, add `DNS_SERVER`
(see [Settings](SETTINGS.md#mediathekenv)).

**Jumping ahead takes a long time.**
Some MKV files have no seek index, so the player reads through the file to get there.
Remuxing writes one: `ffmpeg -i in.mkv -c copy out.mkv`.

**Resuming after a pause takes a moment.**
Resuming seeks again, which is slower on a slow drive.

**A film is listed but does not play.**
Its video codec is one the app cannot decode. See [Formats](PLAYBACK.md#formats).

**Does it come with films?**
No. Use your own legally obtained copies.
