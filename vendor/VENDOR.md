# Vendored libraries

These are pinned, self-contained copies. Do not symlink them from a shared
library store: the build must work from a fresh clone.

| File | Version | Source | License |
|---|---|---|---|
| `termbox2.h` | 2.7.0-dev, commit `48cda6f83a4a1a98d3546beafe55b31a5861c21d` | https://github.com/termbox/termbox2 | MIT (`termbox2.LICENSE`) |
| `miniaudio.h` | v0.11.26 (2026-10-31) | https://github.com/mackron/miniaudio | Public domain / MIT-0 (license at end of header) |
| `minimp3.h` | commit `ea99364f61c14656440e8d77e9c233ccf3124633` | https://github.com/lieff/minimp3 | CC0 (license at top of header) |
| `minimp3_ex.h` | same commit as `minimp3.h` | https://github.com/lieff/minimp3 | CC0 (license at top of header) |
| `stb_vorbis.c` | v1.22 | https://github.com/nothings/stb | Public domain (license at end of file) |
| `maac.h` | 1.0.1 (single-file build) | miniaac 1.0.1 | 0BSD (license at top of file) |
| `minimp4.h` | commit `158a79d8acb8b830fd31beeaa3195aa2269fc04e` | https://github.com/lieff/minimp4 | Public domain / CC0 (license at top of file) |
| `pocketmod.h` | commit `33ac2ba2f7c57aad0944a359e04bde5b2668470d` | https://github.com/rombankzero/pocketmod | MIT (license at end of header) |

`minimp3` is used as the MP3 decoder. `miniaudio` provides the audio device,
mixing, resampling and software volume, and decodes WAV and FLAC. Vorbis is
decoded by `stb_vorbis.c` through miniaudio's built-in backend (`stb_vorbis.c`
lives in `src/vorbis.c` because its static helpers collide with minimp3's).
`maac.h` decodes ADTS AAC. The player does not use miniaudio's built-in MP3
decoder; the data source wrapper in `src/player.c` feeds `minimp3_ex` output
into `ma_sound`. `minimp4.h` demuxes AAC inside M4A; `src/mp4.c` holds its
implementation (its helper names are too generic to share player.c).
`pocketmod.h` renders classic 15/31-sample ProTracker MOD files; its
implementation and the duration probe live in `src/mod.c`.
