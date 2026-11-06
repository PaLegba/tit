# The Iron Tapper
# ~~~>  "minimal console [mp3] music player"
# /// [by Legba]

! WARNING: it is still in development, current state - MVP "v0.1" !

## Principles

* omnia mea mecum porto - I carry everything mine with me.
* 画蛇添足 - don't draw legs on a snake
* UTF-8 for both filenames and id3v1/id3v2 tags
* Fresh modern TUI
* minimal external libraries dependencies
* playlists support
* playlist control: shuffle, ordered by Artist, Track name, Number, Album, reversed play
* `playmp3list-0.99a` ideas and interface layout inspirations, but fresh and modern-looking
* most system code is inherited from single-header and statically linked libraries
* multiplatform: Linux (made), NetBSD (almost), OpenBSD (near), FreeBSD (on the horizon), Haiku (far)
* Build system: suckless Makefile/config.mk one set for all systems
* Static build when it possible

The Iron Tapper is a small local MP3 player for the terminal. The filesystem
is the playlist: start in a directory, walk into subdirectories, select an MP3.
There is no library database, no scanning daemon, no collection and no network
requirement.

## Status

MVP; Linux and NetBSD are tested. It is intentionally small:

* local MP3 playback with `minimp3_ex` and `miniaudio`;
* file/directory/playlist arguments;
* m3u, m3u8 and pls import;
* ID3v1 and ID3v2.2/2.3/2.4 tags, including UTF-16 and legacy CP1251;
* directory browser where the current directory is the play queue;
* shuffle, repeat, artist/title/album/track sort, reverse order;
* remove a track from the in-memory list (the file is never touched);
* software output volume — no system mixer;
* a static set of color themes mirrored from the st config, with a terminal
  default that keeps the terminal's own background;
* null audio backend for testing.

Not in this MVP:

* anything except MP3;
* URL streaming (URL entries from playlists are displayed but not played yet);
* saving/exporting playlists;
* cover art, visualizer, library database, mouse, plugins, MPRIS;
* runtime key bindings (keys are fixed for now);
* OpenBSD/FreeBSD/Haiku packaging and fully static builds.

## Build

Requirements: a C11 compiler, `make`, and the usual POSIX headers. No ncurses,
no libmpg123, no libasound development package: `miniaudio` loads the audio
backend at runtime.

```sh
make
./tit [path ...]
```

`config.mk` holds `PREFIX`, `CC` and `OPT` (`-O2 -g` by default). For a
release build: `make clean && make OPT='-O2'`.

Install:

```sh
make install          # PREFIX=/usr/local by default
make install PREFIX=$HOME/.local
```

The binary is `tit`. `path` may be an MP3 file, a directory, or an `.m3u`,
`.m3u8`, `.pls`, `.lst` or `.list` playlist. With no path, the current
directory is opened. Several MP3 file arguments become a selection queue.

Examples:

```sh
./tit                         # browse current directory
./tit ~/Music                 # browse a directory
./tit album.m3u8              # load a playlist and play its first track
./tit song.mp3 more.mp3       # play these files, then next/prev through them
./tit --no-audio ~/Music      # no sound device needed (UI testing)
./tit --tag-encoding=cp1251 ~/Music   # old Russian tags
./tit --tag-encoding=utf8 ~/Music     # UTF-8 ID3v1/v2.3 tags
./tit --theme=gruvbox ~/Music         # static color theme
./tit --theme=list                    # list themes
./tit --frame ~/Music                 # frame around the list
```

`NO_COLOR=1` disables colors. The UI uses 256-color mode when available.

## Keys

| Key | Action |
|---|---|
| `j` / `k`, Up / Down | move |
| `g` / `G`, Home / End | top / bottom |
| PgUp / PgDn | page |
| Enter | play file, enter directory, load playlist |
| Space | play / pause |
| `n` / `p` | next / previous track |
| Left / Right | seek -5 / +5 seconds |
| `x` | stop |
| `+` / `-` | volume |
| `m` | mute |
| `s` | shuffle |
| `r` | repeat off / all / one |
| `o` | sort: file, artist, title, album, track |
| `O` | reverse order |
| `t` | next color theme |
| `f` | toggle frame around the list |
| `d` | remove highlighted track from the in-memory list |
| `b` / Backspace | parent directory |
| `R` | reload directory / playlist |
| `?` | help |
| `q`, Ctrl-C | quit |

## Notes

* MP3 decoding is `minimp3_ex` (CC0). Output, resampling and software volume
  are `miniaudio` (public domain / MIT-0). Playback does not touch the system
  mixer.
* `--tag-encoding=latin1` is the default per ID3v1/ID3v2.3. Use
  `--tag-encoding=cp1251` for old tags written in CP1251, or
  `--tag-encoding=utf8` for non-standard ID3v1/v2.3 tags written in UTF-8.
* The current directory is the queue. Walking to another directory keeps the
  current playback queue; selecting a track in the new directory makes that
  directory the new queue.
* Every directory view starts with `../`, so the parent directory is always
  visible and selectable. `b` / Backspace do the same thing.
* The header shows the number of playable MP3 tracks, not every row. The
  status line after loading also shows the total number of entries
  (directories, playlists and the `../` row included).
* `f` toggles a single-line frame around the playlist/file browser; `--frame`
  enables it at startup. The default is off.
* `d` only removes a row from memory. It never deletes or renames a file.
* URL entries in playlists are visible but marked `[url]`; they are not
  playable yet.
* Color themes are compiled in; there is no theme parser yet. The default is
  `terminal`, which keeps the terminal's foreground/background. `t` cycles
  through the static set; `--theme=NAME` selects one at startup. Themes are
  mirrored from the `st-0.9.3-new-kitty` config (the shared dwm/st/svkbd set):
  `default`, `light`, `plan9`, `dracula`, `solarized`, `nord`, `gruvbox`,
  `cyberpunk`, `ork`, `ledg`, `ledb`, `ledr`, `ledw`, `ledy`, `leda`, `ledc`,
  `ledmg`, plus `Legba` from the old `playmp3listrc`. Truecolor is used when
  `COLORTERM` advertises it; otherwise colors are mapped to the 256-color
  palette. `TIT_TRUECOLOR=1` forces 24-bit color,
  `TIT_TRUECOLOR=0` forces 256-color mode.
* Linux and NetBSD are the tested targets. OpenBSD, FreeBSD and Haiku are
  planned in that order; static builds are a separate milestone. NetBSD
  build and audio notes are in `netbsd.md`.

## Vendored

Pinned copies live in `vendor/`; see `vendor/VENDOR.md`. The project builds
from a fresh checkout without symlinks to an external library store.

## License

MIT. See `LICENSE` and the licenses in `vendor/`.
