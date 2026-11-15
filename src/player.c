#include "player.h"

#include "mod.h"
#include "pocketmod.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"

#define MAAC_IMPLEMENTATION
#include "maac.h"
#include "minimp4.h"

/* miniaudio has no built-in Vorbis decoder; it drives stb_vorbis when its
   declarations are visible before the implementation. src/vorbis.c holds the
   implementation in a separate translation unit. */
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MA_NO_MP3
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#ifdef __HAIKU__
/* src/haiku_audio.cpp: BSoundPlayer as a miniaudio custom backend */
ma_result tit_haiku_backend_init(ma_backend_callbacks *callbacks);
#endif

#define MOD_RATE 48000

typedef enum {
    SRC_NONE = 0,
    SRC_MP3,
    SRC_DECODER, /* miniaudio: wav, flac, ogg/vorbis */
    SRC_AAC,     /* miniaac: ADTS */
    SRC_MOD      /* pocketmod: ProTracker MOD */
} SourceKind;

struct Player {
    ma_data_source_base ds; /* must be first: the vtable casts to Player */
    ma_engine engine;
    ma_context context;
    bool has_context;
    ma_sound sound;
    bool loaded;
    bool paused;

    SourceKind src;

    mp3dec_ex_t dec;
    bool dec_open;

    pocketmod_context mod;
    bool mod_open;
    bool mod_ended;

    ma_decoder decoder;

    maac_adts adts;
    maac_raw aac_raw;
    maac_bitreader br;
    maac_channel *aac_channels;
    float *aac_pcm;
    uint32_t *aac_offsets;   /* ADTS frame offsets */
    uint64_t *m4a_offsets;   /* MP4 sample offsets */
    unsigned char aac_dsi[64];
    unsigned aac_dsi_len;
    bool aac_adts;
    uint32_t aac_count;
    uint32_t aac_next;
    uint32_t aac_pos;
    uint32_t aac_samples;
    bool aac_have;

    MP4D_demux_t mp4;
    bool mp4_open;

    void *map;
    size_t map_size;

    int channels;
    int hz;
    int bitrate;
    uint64_t total_frames;
    _Atomic uint64_t cursor_frames;
    _Atomic bool ended;
    float volume;
};

static void set_err(char *err, size_t errsize, const char *fmt, ...)
{
    if (!err || !errsize) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsize, fmt, ap);
    va_end(ap);
}

static bool ext_is(const char *path, const char *ext)
{
    size_t np = strlen(path), ne = strlen(ext);
    return np >= ne && strcasecmp(path + np - ne, ext) == 0;
}

/* ---- AAC/ADTS ---------------------------------------------------------- */

static const int aac_rates[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000, 7350,
};

static bool aac_header(const unsigned char *b, size_t len, unsigned *flen,
                       unsigned *cfg, unsigned *sfi)
{
    if (len < 7 || b[0] != 0xff || (b[1] & 0xf6) != 0xf0) return false;
    unsigned length = ((unsigned)(b[3] & 0x03) << 11) |
                      ((unsigned)b[4] << 3) | ((unsigned)b[5] >> 5);
    unsigned rate = (b[2] >> 2) & 0x0f;
    if (length < 7 || rate > 12) return false;
    *flen = length;
    *sfi = rate;
    *cfg = ((unsigned)(b[2] & 0x01) << 2) | ((unsigned)b[3] >> 6);
    return true;
}

static bool aac_alloc_channels(Player *p, int ch, int hz)
{
    if (ch < 1) ch = 2;
    if (ch > 8 || hz <= 0) return false;
    p->channels = ch;
    p->hz = hz;
    p->aac_channels = xcalloc((size_t)ch, sizeof(maac_channel));
    for (int i = 0; i < ch; i++)
        maac_channel_init(&p->aac_channels[i]);
    p->aac_pcm = xmalloc((size_t)ch * 1024 * sizeof(float));
    return true;
}

static bool aac_build_index(Player *p)
{
    const unsigned char *data = p->map;
    size_t pos = 0, cap = 0;
    bool first = true;
    /* offsets are 32-bit (maac bitreader limit) */
    while (pos + 7 <= p->map_size && pos < 0xffffffffu) {
        unsigned flen, cfg, sfi;
        if (!aac_header(data + pos, p->map_size - pos, &flen, &cfg, &sfi)) {
            pos++;
            continue;
        }
        if (first) {
            int ch = cfg == 7 ? 8 : (int)cfg;
            if (!aac_alloc_channels(p, ch, aac_rates[sfi])) return false;
            first = false;
        }
        if (p->aac_count == cap) {
            cap = cap ? cap * 2 : 4096;
            p->aac_offsets = xrealloc(p->aac_offsets,
                                      cap * sizeof(*p->aac_offsets));
        }
        p->aac_offsets[p->aac_count++] = (uint32_t)pos;
        pos += flen;
    }
    p->aac_adts = true;
    return p->aac_count > 0;
}

static bool aac_decode_frame(Player *p)
{
    if (p->aac_next >= p->aac_count) return false;
    size_t off = p->aac_adts ? p->aac_offsets[p->aac_next]
                             : (size_t)p->m4a_offsets[p->aac_next];
    if (off >= p->map_size) return false;
    size_t rest = p->map_size - off;
    /* shortcut: maac's bitreader counts bytes in 32 bits, so >4 GiB AAC
       streams are unsupported */
    if (rest > 0xffffffffu) rest = 0xffffffffu;

    maac_bitreader_init(&p->br);
    p->br.data = (const maac_u8 *)p->map + off;
    p->br.len = (maac_u32)rest;

    if (p->aac_adts) {
        maac_adts_init(&p->adts);
        p->adts.raw.out_channels = p->aac_channels;
        p->adts.raw.num_out_channels = (maac_u8)p->channels;
        if (maac_adts_decode(&p->adts, &p->br) != MAAC_OK) return false;
    } else {
        maac_raw_init(&p->aac_raw);
        if (maac_raw_config(&p->aac_raw, p->aac_dsi,
                            (maac_u32)p->aac_dsi_len) != MAAC_OK)
            return false;
        p->aac_raw.out_channels = p->aac_channels;
        p->aac_raw.num_out_channels = (maac_u8)p->channels;
        if (maac_raw_decode(&p->aac_raw, &p->br) != MAAC_OK) return false;
    }

    uint32_t n = p->aac_channels[0].n_samples;
    if (n == 0 || n > 1024) return false;
    for (uint32_t i = 0; i < n; i++)
        for (int c = 0; c < p->channels; c++)
            /* miniaac emits 16-bit-scaled floats, not [-1, 1] */
            p->aac_pcm[(size_t)i * p->channels + c] =
                (float)(p->aac_channels[c].samples[i] / 32768.0f);
    p->aac_samples = n;
    p->aac_pos = 0;
    p->aac_have = true;
    p->aac_next++;
    return true;
}

static ma_result aac_read(Player *p, void *out, ma_uint64 frames,
                          ma_uint64 *read)
{
    float *dst = out;
    ma_uint64 done = 0;
    while (done < frames) {
        if (!p->aac_have && !aac_decode_frame(p)) break;
        uint64_t avail = p->aac_samples - p->aac_pos;
        ma_uint64 n = frames - done < avail ? frames - done : avail;
        memcpy(dst + done * (size_t)p->channels,
               p->aac_pcm + (size_t)p->aac_pos * p->channels,
               (size_t)n * (size_t)p->channels * sizeof(float));
        p->aac_pos += (uint32_t)n;
        done += n;
        if (p->aac_pos >= p->aac_samples) p->aac_have = false;
    }
    *read = done;
    /* shortcut: assumes every ADTS frame carries 1024 samples */
    if (p->aac_next)
        atomic_store(&p->cursor_frames,
                     (uint64_t)(p->aac_next - 1) * 1024 + p->aac_pos);
    return done ? MA_SUCCESS : MA_AT_END;
}

/* ---- MOD --------------------------------------------------------------- */

static ma_result mod_read(Player *p, void *out, ma_uint64 frames,
                          ma_uint64 *read)
{
    if (!p->mod_open || p->mod_ended) return MA_AT_END;

    float *dst = out;
    ma_uint64 done = 0;
    while (done < frames) {
        ma_uint64 want = frames - done;
        if (want > 4096) want = 4096;
        int got = pocketmod_render(&p->mod, dst + done * 2,
                                   (int)(want * 2 * sizeof(float)));
        int n = got / (int)(sizeof(float) * 2);
        if (n <= 0) break;
        done += (ma_uint64)n;
        if (pocketmod_loop_count(&p->mod) > 0) {
            p->mod_ended = true;
            break;
        }
    }
    *read = done;
    if (!done) return MA_AT_END;
    atomic_store(&p->cursor_frames,
                 atomic_load(&p->cursor_frames) + done);
    return MA_SUCCESS;
}

static ma_result mod_seek(Player *p, ma_uint64 frame)
{
    if (!p->mod_open) return MA_INVALID_OPERATION;
    if (p->total_frames && frame >= p->total_frames)
        frame = p->total_frames - 1;
    if (!pocketmod_init(&p->mod, p->map, (int)p->map_size, MOD_RATE))
        return MA_ERROR;
    p->mod_ended = false;

    /* Pocketmod has no seek; replay into a scratch buffer. */
    float buf[2048 * 2];
    uint64_t left = frame;
    while (left) {
        uint32_t n = left > 2048 ? 2048 : (uint32_t)left;
        int got = pocketmod_render(&p->mod, buf,
                                   (int)(n * 2 * sizeof(float)));
        int rendered = got / (int)(sizeof(float) * 2);
        if (rendered <= 0) break;
        left -= (uint64_t)rendered;
        if (pocketmod_loop_count(&p->mod) > 0) {
            p->mod_ended = true;
            break;
        }
    }
    atomic_store(&p->cursor_frames, frame - left);
    return MA_SUCCESS;
}

/* ---- data source ------------------------------------------------------- */

static ma_result ds_read(ma_data_source *ds, void *out, ma_uint64 frames,
                         ma_uint64 *read)
{
    Player *p = (Player *)ds;
    *read = 0;
    switch (p->src) {
    case SRC_MP3: {
        if (!p->dec_open) return MA_AT_END;
        size_t want = (size_t)frames * (size_t)p->channels;
        size_t got = mp3dec_ex_read(&p->dec, out, want);
        size_t nframes = got / (size_t)p->channels;
        *read = nframes;
        atomic_store(&p->cursor_frames,
                     p->dec.cur_sample / (uint64_t)p->channels);
        if (!got) return MA_AT_END;
        return MA_SUCCESS;
    }
    case SRC_DECODER: {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&p->decoder, out, frames,
                                                 &got);
        *read = got;
        if (got) {
            atomic_store(&p->cursor_frames,
                         atomic_load(&p->cursor_frames) + got);
            return MA_SUCCESS;
        }
        return r;
    }
    case SRC_AAC:
        return aac_read(p, out, frames, read);
    case SRC_MOD:
        return mod_read(p, out, frames, read);
    default:
        return MA_AT_END;
    }
}

static ma_result ds_seek(ma_data_source *ds, ma_uint64 frame)
{
    Player *p = (Player *)ds;
    switch (p->src) {
    case SRC_MP3: {
        if (!p->dec_open) return MA_INVALID_OPERATION;
        uint64_t pos = frame * (uint64_t)p->channels;
        if (p->dec.samples && pos > p->dec.samples) pos = p->dec.samples;
        if (mp3dec_ex_seek(&p->dec, pos) != 0) return MA_ERROR;
        atomic_store(&p->cursor_frames,
                     p->dec.cur_sample / (uint64_t)p->channels);
        return MA_SUCCESS;
    }
    case SRC_DECODER:
        if (ma_decoder_seek_to_pcm_frame(&p->decoder, frame) != MA_SUCCESS)
            return MA_ERROR;
        atomic_store(&p->cursor_frames, frame);
        return MA_SUCCESS;
    case SRC_AAC: {
        uint64_t total = (uint64_t)p->aac_count * 1024;
        if (frame >= total) frame = total ? total - 1 : 0;
        uint32_t idx = (uint32_t)(frame / 1024);
        uint32_t off = (uint32_t)(frame % 1024);
        p->aac_next = idx;
        p->aac_have = false;
        if (!aac_decode_frame(p)) return MA_ERROR;
        if (off >= p->aac_samples) off = p->aac_samples - 1;
        p->aac_pos = off;
        atomic_store(&p->cursor_frames, frame);
        return MA_SUCCESS;
    }
    case SRC_MOD:
        return mod_seek(p, frame);
    default:
        return MA_INVALID_OPERATION;
    }
}

static ma_result ds_format(ma_data_source *ds, ma_format *format,
                           ma_uint32 *channels, ma_uint32 *hz,
                           ma_channel *map, size_t map_cap)
{
    Player *p = (Player *)ds;
    (void)map;
    (void)map_cap;
    if (format) *format = ma_format_f32;
    if (channels) *channels = (ma_uint32)p->channels;
    if (hz) *hz = (ma_uint32)p->hz;
    return MA_SUCCESS;
}

static ma_result ds_cursor(ma_data_source *ds, ma_uint64 *cursor)
{
    Player *p = (Player *)ds;
    *cursor = atomic_load(&p->cursor_frames);
    return MA_SUCCESS;
}

static ma_result ds_length(ma_data_source *ds, ma_uint64 *length)
{
    Player *p = (Player *)ds;
    if (!p->total_frames) return MA_NOT_IMPLEMENTED;
    *length = p->total_frames;
    return MA_SUCCESS;
}

static ma_data_source_vtable ds_vtable = {
    .onRead = ds_read,
    .onSeek = ds_seek,
    .onGetDataFormat = ds_format,
    .onGetCursor = ds_cursor,
    .onGetLength = ds_length,
};

static void on_end(void *user, ma_sound *sound)
{
    (void)sound;
    Player *p = user;
    atomic_store(&p->ended, true);
}

Player *player_create(bool null_audio, char *err, size_t errsize)
{
    Player *p = xcalloc(1, sizeof(*p));
    p->volume = 0.7f;

    ma_data_source_config dsc = ma_data_source_config_init();
    dsc.vtable = &ds_vtable;
    if (ma_data_source_init(&dsc, &p->ds) != MA_SUCCESS) {
        set_err(err, errsize, "cannot initialize audio data source");
        free(p);
        return NULL;
    }

    ma_engine_config ec = ma_engine_config_init();
    ec.channels = 2;
    ec.periodSizeInMilliseconds = 20;
    if (null_audio) {
        ma_backend backend = ma_backend_null;
        if (ma_context_init(&backend, 1, NULL, &p->context) != MA_SUCCESS) {
            set_err(err, errsize, "cannot initialize null audio backend");
            ma_data_source_uninit(&p->ds);
            free(p);
            return NULL;
        }
        p->has_context = true;
        ec.pContext = &p->context;
    }
#ifdef __HAIKU__
    else {
        ma_backend_callbacks callbacks;
        ma_backend backends[2] = { ma_backend_custom, ma_backend_null };
        ma_context_config ccfg = ma_context_config_init();
        if (tit_haiku_backend_init(&callbacks) == MA_SUCCESS) {
            ccfg.custom = callbacks;
            if (ma_context_init(backends, 2, &ccfg, &p->context) == MA_SUCCESS) {
                p->has_context = true;
                ec.pContext = &p->context;
            }
        }
    }
#endif
    ma_result r = ma_engine_init(&ec, &p->engine);
#ifdef __HAIKU__
    if (r != MA_SUCCESS && p->has_context) {
        /* Media Kit had an output during the probe but not during device
           init; retry with miniaudio's default (Null on Haiku). */
        ma_context_uninit(&p->context);
        p->has_context = false;
        ec.pContext = NULL;
        r = ma_engine_init(&ec, &p->engine);
    }
#endif
    if (r != MA_SUCCESS) {
        set_err(err, errsize, "cannot initialize audio device (error %d); "
                              "try --no-audio", (int)r);
        if (p->has_context) ma_context_uninit(&p->context);
        ma_data_source_uninit(&p->ds);
        free(p);
        return NULL;
    }
    return p;
}

static void close_track(Player *p)
{
    if (p->loaded) {
        ma_sound_stop(&p->sound);
        ma_sound_uninit(&p->sound);
        p->loaded = false;
    }
    if (p->src == SRC_MP3 && p->dec_open) {
        mp3dec_ex_close(&p->dec);
        p->dec_open = false;
    }
    if (p->src == SRC_DECODER) ma_decoder_uninit(&p->decoder);
    free(p->aac_channels);
    p->aac_channels = NULL;
    free(p->aac_pcm);
    p->aac_pcm = NULL;
    free(p->aac_offsets);
    p->aac_offsets = NULL;
    free(p->m4a_offsets);
    p->m4a_offsets = NULL;
    if (p->mp4_open) {
        MP4D_close(&p->mp4);
        p->mp4_open = false;
    }
    p->aac_dsi_len = 0;
    p->aac_adts = false;
    p->aac_count = 0;
    p->aac_next = 0;
    p->aac_pos = 0;
    p->aac_samples = 0;
    p->aac_have = false;
    p->mod_open = false;
    p->mod_ended = false;
    memset(&p->mod, 0, sizeof(p->mod));
    if (p->map) {
        munmap(p->map, p->map_size);
        p->map = NULL;
        p->map_size = 0;
    }
    p->src = SRC_NONE;
    p->paused = false;
    p->channels = 0;
    p->hz = 0;
    p->bitrate = 0;
    p->total_frames = 0;
    atomic_store(&p->cursor_frames, 0);
    atomic_store(&p->ended, false);
}

void player_close(Player *p)
{
    if (p) close_track(p);
}

static int start_sound(Player *p, char *err, size_t errsize)
{
    ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
    ma_result r = ma_sound_init_from_data_source(&p->engine, &p->ds, flags,
                                                  NULL, &p->sound);
    if (r != MA_SUCCESS) {
        set_err(err, errsize, "cannot initialize sound (error %d)", (int)r);
        close_track(p);
        return -1;
    }
    p->loaded = true;
    p->paused = false;
    ma_sound_set_end_callback(&p->sound, on_end, p);
    ma_sound_set_volume(&p->sound, p->volume);
    r = ma_sound_start(&p->sound);
    if (r != MA_SUCCESS) {
        set_err(err, errsize, "cannot start playback (error %d)", (int)r);
        close_track(p);
        return -1;
    }
    return 0;
}

static int open_mp3(Player *p, const char *path, char *err, size_t errsize)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_err(err, errsize, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 4) {
        close(fd);
        set_err(err, errsize, "not a regular file: %s", path);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        set_err(err, errsize, "cannot map %s: %s", path, strerror(errno));
        return -1;
    }
    if (mp3dec_ex_open_buf(&p->dec, map, size, MP3D_SEEK_TO_SAMPLE) != 0) {
        mp3dec_ex_close(&p->dec);
        munmap(map, size);
        set_err(err, errsize, "cannot decode %s: unsupported or corrupt MP3",
                path);
        return -1;
    }
    p->map = map;
    p->map_size = size;
    p->src = SRC_MP3;
    p->dec_open = true;
    p->channels = p->dec.info.channels;
    p->hz = p->dec.info.hz;
    p->bitrate = p->dec.info.bitrate_kbps;
    if (p->channels <= 0 || p->hz <= 0) {
        set_err(err, errsize, "no MP3 audio frames in %s", path);
        close_track(p);
        return -1;
    }
    if (p->dec.samples && !p->dec.indexes_built) {
        /* Build the seek index on the UI thread, not in the audio callback. */
        mp3dec_ex_seek(&p->dec, 1);
        mp3dec_ex_seek(&p->dec, 0);
    }
    p->total_frames = p->dec.samples / (uint64_t)p->channels;
    atomic_store(&p->cursor_frames, 0);
    atomic_store(&p->ended, false);
    return start_sound(p, err, errsize);
}

static int open_decoder(Player *p, const char *path, char *err, size_t errsize)
{
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_result r = ma_decoder_init_file(path, &cfg, &p->decoder);
    if (r != MA_SUCCESS) {
        set_err(err, errsize, "cannot decode %s: unsupported or corrupt audio",
                path);
        return -1;
    }
    p->src = SRC_DECODER;

    ma_uint32 ch = 0, hz = 0;
    ma_decoder_get_data_format(&p->decoder, NULL, &ch, &hz, NULL, 0);
    p->channels = (int)ch;
    p->hz = (int)hz;
    if (p->channels <= 0 || p->hz <= 0) {
        set_err(err, errsize, "no audio frames in %s", path);
        close_track(p);
        return -1;
    }
    ma_uint64 len = 0;
    if (ma_decoder_get_length_in_pcm_frames(&p->decoder, &len) == MA_SUCCESS)
        p->total_frames = len;

    struct stat st;
    if (p->total_frames && stat(path, &st) == 0 && st.st_size > 0) {
        p->bitrate = (int)((uint64_t)st.st_size * 8 * (uint64_t)p->hz /
                           p->total_frames / 1000);
    }
    atomic_store(&p->cursor_frames, 0);
    atomic_store(&p->ended, false);
    return start_sound(p, err, errsize);
}

static int open_aac(Player *p, const char *path, char *err, size_t errsize)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_err(err, errsize, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 7) {
        close(fd);
        set_err(err, errsize, "not a regular file: %s", path);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        set_err(err, errsize, "cannot map %s: %s", path, strerror(errno));
        return -1;
    }
    p->map = map;
    p->map_size = size;
    p->src = SRC_AAC;
    if (!aac_build_index(p)) {
        set_err(err, errsize, "no AAC/ADTS frames in %s", path);
        close_track(p);
        return -1;
    }
    p->total_frames = (uint64_t)p->aac_count * 1024;
    struct stat fs;
    if (stat(path, &fs) == 0 && fs.st_size > 0 && p->total_frames && p->hz) {
        p->bitrate = (int)((uint64_t)fs.st_size * 8 * (uint64_t)p->hz /
                           p->total_frames / 1000);
    }
    atomic_store(&p->cursor_frames, 0);
    atomic_store(&p->ended, false);
    return start_sound(p, err, errsize);
}

static int m4a_read(int64_t offset, void *buffer, size_t size, void *token)
{
    Player *p = token;
    if (offset < 0 || (uint64_t)offset > (uint64_t)p->map_size) return 1;
    size_t avail = p->map_size - (size_t)offset;
    size_t got = size < avail ? size : avail;
    memcpy(buffer, (const unsigned char *)p->map + (size_t)offset, got);
    return got != size;
}

static int open_m4a(Player *p, const char *path, char *err, size_t errsize)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_err(err, errsize, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 8) {
        close(fd);
        set_err(err, errsize, "not a regular file: %s", path);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        set_err(err, errsize, "cannot map %s: %s", path, strerror(errno));
        return -1;
    }
    p->map = map;
    p->map_size = size;
    p->src = SRC_AAC;
    p->aac_adts = false;

    memset(&p->mp4, 0, sizeof(p->mp4));
    if (!MP4D_open(&p->mp4, m4a_read, p, (int64_t)size)) {
        set_err(err, errsize, "cannot parse M4A: %s", path);
        close_track(p);
        return -1;
    }
    p->mp4_open = true;

    int track = -1;
    for (unsigned i = 0; i < p->mp4.track_count; i++) {
        MP4D_track_t *tr = &p->mp4.track[i];
        if (tr->handler_type != MP4D_HANDLER_TYPE_SOUN) continue;
        if (!tr->dsi || !tr->dsi_bytes) continue;
        switch (tr->object_type_indication) {
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3:
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_MAIN_PROFILE:
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_LC_PROFILE:
        case MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_SSR_PROFILE:
            track = (int)i;
            break;
        }
        if (track >= 0) break;
    }
    if (track < 0) {
        set_err(err, errsize,
                "no AAC audio track in %s (ALAC is not supported)", path);
        close_track(p);
        return -1;
    }

    MP4D_track_t *tr = &p->mp4.track[track];
    if (tr->dsi_bytes > sizeof(p->aac_dsi)) {
        set_err(err, errsize, "unsupported M4A decoder config in %s", path);
        close_track(p);
        return -1;
    }
    if (tr->sample_count == 0) {
        set_err(err, errsize,
                "fragmented or empty M4A is not supported: %s", path);
        close_track(p);
        return -1;
    }
    p->aac_dsi_len = tr->dsi_bytes;
    memcpy(p->aac_dsi, tr->dsi, p->aac_dsi_len);

    maac_raw_init(&p->aac_raw);
    if (maac_raw_config(&p->aac_raw, p->aac_dsi,
                        (maac_u32)p->aac_dsi_len) != MAAC_OK) {
        set_err(err, errsize,
                "unsupported M4A codec in %s (AAC-LC only)", path);
        close_track(p);
        return -1;
    }
    int ch = p->aac_raw.channel_configuration == 7
        ? 8 : (int)p->aac_raw.channel_configuration;
    if (ch < 1) ch = (int)tr->SampleDescription.audio.channelcount;
    if (!aac_alloc_channels(p, ch, (int)p->aac_raw.sample_rate)) {
        set_err(err, errsize, "unsupported M4A audio format in %s", path);
        close_track(p);
        return -1;
    }

    p->aac_count = tr->sample_count;
    p->m4a_offsets = xmalloc((size_t)p->aac_count *
                             sizeof(*p->m4a_offsets));
    for (uint32_t i = 0; i < p->aac_count; i++) {
        unsigned bytes = 0, timestamp = 0, duration = 0;
        MP4D_file_offset_t off = MP4D_frame_offset(
            &p->mp4, (unsigned)track, i, &bytes, &timestamp, &duration);
        if (off >= (uint64_t)p->map_size || bytes == 0 ||
            (uint64_t)bytes > (uint64_t)p->map_size - off) {
            set_err(err, errsize, "broken M4A sample table in %s", path);
            close_track(p);
            return -1;
        }
        p->m4a_offsets[i] = off;
    }

    p->total_frames = (uint64_t)p->aac_count * 1024;
    struct stat fs;
    if (stat(path, &fs) == 0 && fs.st_size > 0 && p->total_frames && p->hz) {
        p->bitrate = (int)((uint64_t)fs.st_size * 8 * (uint64_t)p->hz /
                           p->total_frames / 1000);
    }
    atomic_store(&p->cursor_frames, 0);
    atomic_store(&p->ended, false);
    return start_sound(p, err, errsize);
}

static int open_mod(Player *p, const char *path, char *err, size_t errsize)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_err(err, errsize, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 600) {
        close(fd);
        set_err(err, errsize, "not a regular MOD file: %s", path);
        return -1;
    }
    if (st.st_size > INT_MAX) {
        close(fd);
        set_err(err, errsize, "MOD file is too large: %s", path);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        set_err(err, errsize, "cannot map %s: %s", path, strerror(errno));
        return -1;
    }

    p->map = map;
    p->map_size = size;
    p->src = SRC_MOD;
    p->channels = 2;
    p->hz = MOD_RATE;
    if (!pocketmod_init(&p->mod, map, (int)size, MOD_RATE)) {
        set_err(err, errsize,
                "cannot decode %s: unsupported or corrupt MOD", path);
        close_track(p);
        return -1;
    }
    p->mod_open = true;
    p->total_frames = mod_duration_frames(map, size, MOD_RATE);
    p->bitrate = 0;
    atomic_store(&p->cursor_frames, 0);
    atomic_store(&p->ended, false);
    return start_sound(p, err, errsize);
}

int player_open(Player *p, const char *path, char *err, size_t errsize)
{
    close_track(p);

    if (is_mp3_name(path)) return open_mp3(p, path, err, errsize);
    if (ext_is(path, ".m4a")) return open_m4a(p, path, err, errsize);
    if (ext_is(path, ".aac")) return open_aac(p, path, err, errsize);
    if (ext_is(path, ".mod")) return open_mod(p, path, err, errsize);
    if (ext_is(path, ".wav") || ext_is(path, ".flac") ||
        ext_is(path, ".ogg") || ext_is(path, ".oga"))
        return open_decoder(p, path, err, errsize);
    set_err(err, errsize, "unsupported audio format: %s", path);
    return -1;
}

int player_play(Player *p)
{
    if (!p->loaded) return -1;
    ma_result r = ma_sound_start(&p->sound);
    if (r == MA_SUCCESS) p->paused = false;
    return r == MA_SUCCESS ? 0 : -1;
}

int player_pause(Player *p)
{
    if (!p->loaded) return -1;
    ma_result r = ma_sound_stop(&p->sound);
    if (r == MA_SUCCESS) p->paused = true;
    return r == MA_SUCCESS ? 0 : -1;
}

int player_stop(Player *p)
{
    if (!p->loaded) return -1;
    ma_sound_stop(&p->sound);
    ma_sound_seek_to_pcm_frame(&p->sound, 0);
    p->paused = false;
    atomic_store(&p->cursor_frames, 0);
    atomic_store(&p->ended, false);
    return 0;
}

bool player_has_track(const Player *p)
{
    return p->loaded;
}

bool player_is_playing(const Player *p)
{
    return p->loaded && !p->paused && !atomic_load(&p->ended) &&
           ma_sound_is_playing(&p->sound);
}

bool player_is_paused(const Player *p)
{
    return p->loaded && p->paused;
}

bool player_take_ended(Player *p)
{
    return atomic_exchange(&p->ended, false);
}

void player_seek_frames(Player *p, uint64_t frame)
{
    if (!p->loaded) return;
    if (p->total_frames && frame >= p->total_frames)
        frame = p->total_frames ? p->total_frames - 1 : 0;
    ma_sound_seek_to_pcm_frame(&p->sound, frame);
    atomic_store(&p->cursor_frames, frame);
    atomic_store(&p->ended, false);
}

void player_seek_ms(Player *p, int64_t delta_ms)
{
    if (!p->loaded || !p->hz) return;
    int64_t delta = delta_ms * p->hz / 1000;
    int64_t pos = (int64_t)atomic_load(&p->cursor_frames) + delta;
    if (pos < 0) pos = 0;
    player_seek_frames(p, (uint64_t)pos);
}

uint64_t player_pos_frames(const Player *p)
{
    return atomic_load(&p->cursor_frames);
}

uint64_t player_len_frames(const Player *p)
{
    return p->total_frames;
}

int player_hz(const Player *p)
{
    return p->hz;
}

int player_channels(const Player *p)
{
    return p->channels;
}

int player_bitrate(const Player *p)
{
    return p->bitrate;
}

void player_set_volume(Player *p, float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    p->volume = volume;
    if (p->loaded) ma_sound_set_volume(&p->sound, volume);
}

float player_volume(const Player *p)
{
    return p->volume;
}

void player_destroy(Player *p)
{
    if (!p) return;
    close_track(p);
    ma_data_source_uninit(&p->ds);
    ma_engine_uninit(&p->engine);
    if (p->has_context) ma_context_uninit(&p->context);
    free(p);
}
