#ifndef TIT_PLAYLIST_H
#define TIT_PLAYLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tags.h"

typedef enum {
    ENT_PARENT,
    ENT_DIR,
    ENT_PLAYLIST,
    ENT_FILE,
    ENT_URL
} EntryType;

typedef enum {
    SORT_FILE,
    SORT_ARTIST,
    SORT_TITLE,
    SORT_ALBUM,
    SORT_TRACK
} SortMode;

typedef struct {
    char *path;
    char *name;
    EntryType type;
    TrackMeta meta;
    bool missing;
    uint32_t seq;
} Entry;

typedef struct {
    Entry *items;
    size_t len;
    size_t cap;
    char *source;
    bool is_playlist;
    bool is_selection;
} View;

void view_init(View *v);
void view_free(View *v);

int view_load_dir(View *v, const char *path, TagEncoding enc);
int view_load_playlist(View *v, const char *path, TagEncoding enc);
int view_load_files(View *v, char *const *paths, size_t n, TagEncoding enc);

void view_sort(View *v, SortMode mode, bool reverse);
bool view_entry_playable(const Entry *e);
size_t view_count_tracks(const View *v);
int view_find_path(const View *v, const char *path);
void view_remove(View *v, size_t index);
const char *view_title(const View *v);

#endif
