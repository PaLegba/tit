#ifndef TIT_APP_H
#define TIT_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "player.h"
#include "playlist.h"
#include "tags.h"

typedef enum {
    REPEAT_OFF,
    REPEAT_ALL,
    REPEAT_ONE
} RepeatMode;

typedef struct {
    char *path;
    TrackMeta meta;
} QTrack;

typedef struct {
    QTrack *items;
    size_t len;
    size_t cap;
    size_t pos;
} Queue;

typedef struct {
    View view;
    Queue queue;
    Player *player;
    TagEncoding tag_enc;
    SortMode sort;
    bool reverse;
    bool shuffle;
    RepeatMode repeat;
    size_t cursor;
    size_t top;
    int playing_row;
    float last_volume;
    char status[256];
    uint64_t status_until;
    bool quit;
    bool help;
} App;

void app_init(App *a, Player *player, TagEncoding enc);
void app_free(App *a);

void app_open_dir(App *a, const char *path);
void app_open_path(App *a, const char *path, bool autoplay);
void app_open_files(App *a, char *const *paths, size_t n);

void app_activate(App *a);
void app_back(App *a);
void app_reload(App *a);
void app_move(App *a, int delta);
void app_home(App *a);
void app_end(App *a);
void app_page(App *a, int delta, int page);

void app_play_pause(App *a);
void app_stop(App *a);
void app_next(App *a);
void app_prev(App *a);
void app_seek(App *a, int64_t delta_ms);
void app_volume_delta(App *a, int delta);
void app_mute(App *a);
void app_toggle_shuffle(App *a);
void app_cycle_sort(App *a);
void app_toggle_reverse(App *a);
void app_cycle_repeat(App *a);
void app_delete(App *a);

void app_tick(App *a);
void app_note(App *a, const char *text);
const QTrack *app_now(const App *a);
const char *app_status(const App *a);
const char *app_sort_name(SortMode mode);
const char *app_repeat_name(RepeatMode mode);

#endif
