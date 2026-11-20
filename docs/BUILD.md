# Building from source

```bash
sudo apt install nasm # dav1d's assembly; clang-19, lld-19, ninja and python3 too
make                  # the PS5 app folder in dist/PPSA66120; fetches its toolchain
make test-unit        # GoogleTest, sanitizers on
make format-check
make lint             # takes a few minutes

python3 tools/make-sample-movies.py                 # the invented films and series for the preview
tools/host-snapshots.sh build/snapshots mediathek   # PC preview: a PNG of every screen
HUI_LANG=de tools/host-snapshots.sh build/snapshots-de mediathek   # the same in German
```

The build fetches dav1d 1.5.4 and Meson (pinned, checked) into `.deps/dav1d` and builds
dav1d there, once for the console and once for the tests.

Copy `.env.example` to `.env` for your console's address (`make deploy`) and your TMDB
key. The preview renders the same UI code with Mesa, with no sound and a test picture
in place of video. `python3 tools/fetch-posters.py /path/to/movies` fetches posters on
a PC (`--dry-run` lists what is missing).

Mediathek is a native C++20 app drawn with OpenGL 4.6, built on the
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) kit. See
[AGENTS.md](../AGENTS.md) and [CONTRIBUTING.md](../CONTRIBUTING.md) for the code and the
rules for changes. A release tag is the exact `contentVersion` of `sce_sys/param.json`.
