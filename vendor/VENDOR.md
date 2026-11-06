# Vendored libraries

These are pinned, self-contained copies. Do not symlink them from a shared
library store: the build must work from a fresh clone.

| File | Version | Source | License |
|---|---|---|---|
| `termbox2.h` | 2.7.0-dev, commit `48cda6f83a4a1a98d3546beafe55b31a5861c21d` | https://github.com/termbox/termbox2 | MIT (`termbox2.LICENSE`) |
| `miniaudio.h` | v0.11.26 (2026-10-31) | https://github.com/mackron/miniaudio | Public domain / MIT-0 (license at end of header) |
| `minimp3.h` | commit `ea99364f61c14656440e8d77e9c233ccf3124633` | https://github.com/lieff/minimp3 | CC0 (license at top of header) |
| `minimp3_ex.h` | same commit as `minimp3.h` | https://github.com/lieff/minimp3 | CC0 (license at top of header) |

`minimp3` is used as the MP3 decoder. `miniaudio` provides the audio device,
mixing, resampling and software volume. The player does not use miniaudio's
built-in MP3 decoder; the data source wrapper in `src/player.c` feeds
`minimp3_ex` output into `ma_sound`.
