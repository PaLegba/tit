#include "player.h"

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"

#define MA_NO_MP3
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

struct Player {
    ma_data_source_base ds; /* must be first: the vtable casts to Player */
    ma_engine engine;
    ma_context context;
    bool has_context;
    ma_sound sound;
    bool loaded;
    bool paused;

    mp3dec_ex_t dec;
    bool dec_open;
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

static ma_result ds_read(ma_data_source *ds, void *out, ma_uint64 frames,
                         ma_uint64 *read)
{
    Player *p = (Player *)ds;
    if (!p->dec_open) {
        *read = 0;
        return MA_AT_END;
    }
    size_t want = (size_t)frames * (size_t)p->channels;
    size_t got = mp3dec_ex_read(&p->dec, out, want);
    size_t nframes = got / (size_t)p->channels;
    *read = nframes;
    atomic_store(&p->cursor_frames, p->dec.cur_sample / (uint64_t)p->channels);
    if (!got) return MA_AT_END;
    return MA_SUCCESS;
}

static ma_result ds_seek(ma_data_source *ds, ma_uint64 frame)
{
    Player *p = (Player *)ds;
    if (!p->dec_open) return MA_INVALID_OPERATION;
    uint64_t pos = frame * (uint64_t)p->channels;
    if (p->dec.samples && pos > p->dec.samples) pos = p->dec.samples;
    if (mp3dec_ex_seek(&p->dec, pos) != 0) return MA_ERROR;
    atomic_store(&p->cursor_frames, p->dec.cur_sample / (uint64_t)p->channels);
    return MA_SUCCESS;
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
    ma_result r = ma_engine_init(&ec, &p->engine);
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
    if (p->dec_open) {
        mp3dec_ex_close(&p->dec);
        p->dec_open = false;
    }
    if (p->map) {
        munmap(p->map, p->map_size);
        p->map = NULL;
        p->map_size = 0;
    }
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

int player_open(Player *p, const char *path, char *err, size_t errsize)
{
    close_track(p);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_err(err, errsize, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 4) {
        close(fd);
        set_err(err, errsize, "not a regular MP3 file: %s", path);
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
