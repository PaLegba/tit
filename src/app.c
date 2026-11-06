#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

static void queue_clear(Queue *q)
{
    for (size_t i = 0; i < q->len; i++) {
        free(q->items[i].path);
        meta_free(&q->items[i].meta);
    }
    free(q->items);
    memset(q, 0, sizeof(*q));
}

static void queue_push(Queue *q, const Entry *e)
{
    if (q->len == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 32;
        q->items = xrealloc(q->items, q->cap * sizeof(*q->items));
    }
    QTrack *t = &q->items[q->len++];
    t->path = xstrdup(e->path);
    meta_copy(&t->meta, &e->meta);
}

static int queue_find_path(const Queue *q, const char *path)
{
    for (size_t i = 0; i < q->len; i++)
        if (strcmp(q->items[i].path, path) == 0) return (int)i;
    return -1;
}

static void queue_remove_at(Queue *q, size_t idx)
{
    if (idx >= q->len) return;
    free(q->items[idx].path);
    meta_free(&q->items[idx].meta);
    memmove(&q->items[idx], &q->items[idx + 1],
            (q->len - idx - 1) * sizeof(*q->items));
    q->len--;
    if (idx < q->pos && q->pos > 0) q->pos--;
    if (q->pos >= q->len) q->pos = q->len ? q->len - 1 : 0;
}

static void set_status(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof(a->status), fmt, ap);
    va_end(ap);
    a->status_until = now_ms() + 3500;
}

static void update_playing_row(App *a)
{
    const QTrack *t = app_now(a);
    a->playing_row = t ? view_find_path(&a->view, t->path) : -1;
}

static size_t count_playable(const View *v)
{
    size_t n = 0;
    for (size_t i = 0; i < v->len; i++)
        if (view_entry_playable(&v->items[i])) n++;
    return n;
}

static void queue_build(App *a, size_t start_row, bool shuffle)
{
    queue_clear(&a->queue);
    size_t n = count_playable(&a->view);
    if (!n) {
        update_playing_row(a);
        return;
    }
    size_t *rows = xmalloc(n * sizeof(*rows));
    size_t k = 0;
    for (size_t i = 0; i < a->view.len; i++)
        if (view_entry_playable(&a->view.items[i]))
            rows[k++] = i;

    if (shuffle) {
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = (size_t)rand() % (i + 1);
            size_t tmp = rows[i];
            rows[i] = rows[j];
            rows[j] = tmp;
        }
    }

    size_t start_pos = 0;
    if (start_row < a->view.len) {
        for (size_t i = 0; i < n; i++) {
            if (rows[i] == start_row) {
                start_pos = i;
                break;
            }
        }
        if (shuffle && start_pos != 0) {
            size_t tmp = rows[0];
            rows[0] = rows[start_pos];
            rows[start_pos] = tmp;
            start_pos = 0;
        }
    }
    for (size_t i = 0; i < n; i++)
        queue_push(&a->queue, &a->view.items[rows[i]]);
    a->queue.pos = start_pos;
    free(rows);
    update_playing_row(a);
}

static bool queue_play(App *a, size_t pos, bool skip_bad)
{
    char err[256];
    while (pos < a->queue.len) {
        err[0] = '\0';
        if (player_open(a->player, a->queue.items[pos].path, err, sizeof(err)) == 0) {
            a->queue.pos = pos;
            update_playing_row(a);
            return true;
        }
        set_status(a, "%s", err[0] ? err : "cannot play track");
        if (!skip_bad) {
            a->queue.pos = pos;
            a->playing_row = -1;
            return false;
        }
        pos++;
    }
    player_close(a->player);
    a->playing_row = -1;
    return false;
}

static void play_row(App *a, size_t row)
{
    if (row >= a->view.len) return;
    Entry *e = &a->view.items[row];
    if (e->type == ENT_URL) {
        set_status(a, "streams are not supported yet");
        return;
    }
    if (e->type == ENT_FILE && e->missing) {
        set_status(a, "missing file: %s", e->path);
        return;
    }
    if (!view_entry_playable(e)) {
        set_status(a, "not an MP3 file");
        return;
    }
    queue_build(a, row, a->shuffle);
    if (!a->queue.len) {
        set_status(a, "nothing to play");
        return;
    }
    if (!queue_play(a, a->queue.pos, false))
        return;
}

static void open_dir(App *a, const char *path)
{
    if (view_load_dir(&a->view, path, a->tag_enc) != 0) {
        set_status(a, "cannot open directory: %s", path);
        return;
    }
    view_sort(&a->view, a->sort, a->reverse);
    a->cursor = 0;
    a->top = 0;
    update_playing_row(a);
    size_t tracks = view_count_tracks(&a->view);
    set_status(a, "%zu track%s, %zu entr%s", tracks, tracks == 1 ? "" : "s",
               a->view.len, a->view.len == 1 ? "y" : "ies");
}

static void open_playlist(App *a, const char *path, bool autoplay)
{
    if (view_load_playlist(&a->view, path, a->tag_enc) != 0) {
        set_status(a, "cannot open playlist: %s", path);
        return;
    }
    view_sort(&a->view, a->sort, a->reverse);
    a->cursor = 0;
    a->top = 0;
    update_playing_row(a);
    size_t tracks = view_count_tracks(&a->view);
    set_status(a, "playlist: %zu track%s, %zu entr%s",
               tracks, tracks == 1 ? "" : "s",
               a->view.len, a->view.len == 1 ? "y" : "ies");
    if (autoplay) {
        for (size_t i = 0; i < a->view.len; i++) {
            if (view_entry_playable(&a->view.items[i])) {
                a->cursor = i;
                play_row(a, i);
                break;
            }
        }
    }
}

void app_init(App *a, Player *player, TagEncoding enc)
{
    memset(a, 0, sizeof(*a));
    view_init(&a->view);
    a->player = player;
    a->tag_enc = enc;
    a->sort = SORT_FILE;
    a->repeat = REPEAT_OFF;
    a->last_volume = 0.7f;
    a->playing_row = -1;
    srand((unsigned)(time(NULL) ^ (pid_t)getpid()));
}

void app_free(App *a)
{
    queue_clear(&a->queue);
    view_free(&a->view);
}

void app_open_dir(App *a, const char *path)
{
    open_dir(a, path);
}

void app_open_path(App *a, const char *path, bool autoplay)
{
    if (!path || !*path) {
        open_dir(a, ".");
        return;
    }
    if (is_url(path)) {
        set_status(a, "streams are not supported yet");
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        set_status(a, "no such file or directory: %s", path);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        open_dir(a, path);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        set_status(a, "not a regular file: %s", path);
        return;
    }
    if (is_playlist_name(path)) {
        open_playlist(a, path, autoplay);
        return;
    }
    if (is_mp3_name(path)) {
        char *abs = path_absolute(path);
        char *dir = path_dirname(abs);
        if (view_load_dir(&a->view, dir, a->tag_enc) != 0) {
            set_status(a, "cannot open directory: %s", dir);
        } else {
            view_sort(&a->view, a->sort, a->reverse);
            int row = view_find_path(&a->view, abs);
            a->cursor = row >= 0 ? (size_t)row : 0;
            a->top = 0;
            update_playing_row(a);
            if (autoplay && row >= 0) play_row(a, (size_t)row);
        }
        free(dir);
        free(abs);
        return;
    }
    set_status(a, "only .mp3 files are supported");
}

void app_open_files(App *a, char *const *paths, size_t n)
{
    if (view_load_files(&a->view, paths, n, a->tag_enc) != 0) {
        set_status(a, "no playable files in arguments");
        return;
    }
    view_sort(&a->view, a->sort, a->reverse);
    a->cursor = 0;
    a->top = 0;
    update_playing_row(a);
    for (size_t i = 0; i < a->view.len; i++) {
        if (view_entry_playable(&a->view.items[i])) {
            play_row(a, i);
            break;
        }
    }
}

void app_activate(App *a)
{
    if (a->cursor >= a->view.len) return;
    Entry *e = &a->view.items[a->cursor];
    switch (e->type) {
    case ENT_PARENT:
        app_back(a);
        break;
    case ENT_DIR:
        open_dir(a, e->path);
        break;
    case ENT_PLAYLIST:
        open_playlist(a, e->path, false);
        break;
    case ENT_FILE:
        play_row(a, a->cursor);
        break;
    case ENT_URL:
        set_status(a, "streams are not supported yet");
        break;
    }
}

void app_back(App *a)
{
    if (a->view.is_selection) {
        open_dir(a, ".");
    } else if (a->view.is_playlist) {
        char *dir = path_dirname(a->view.source ? a->view.source : ".");
        open_dir(a, dir);
        free(dir);
    } else {
        char *parent = path_parent(a->view.source ? a->view.source : ".");
        open_dir(a, parent);
        free(parent);
    }
}

void app_reload(App *a)
{
    if (a->view.is_selection) {
        set_status(a, "cannot reload a selection");
        return;
    }
    if (!a->view.source) {
        set_status(a, "nothing to reload");
        return;
    }
    char *keep = NULL;
    if (a->cursor < a->view.len)
        keep = xstrdup(a->view.items[a->cursor].path);
    if (a->view.is_playlist) {
        char *source = xstrdup(a->view.source);
        open_playlist(a, source, false);
        free(source);
    } else {
        char *source = xstrdup(a->view.source);
        open_dir(a, source);
        free(source);
    }
    if (keep) {
        int row = view_find_path(&a->view, keep);
        if (row >= 0) a->cursor = (size_t)row;
        free(keep);
    }
    update_playing_row(a);
}

void app_move(App *a, int delta)
{
    if (!a->view.len) return;
    long v = (long)a->cursor + delta;
    if (v < 0) v = 0;
    if ((size_t)v >= a->view.len) v = (long)a->view.len - 1;
    a->cursor = (size_t)v;
}

void app_home(App *a)
{
    a->cursor = 0;
    a->top = 0;
}

void app_end(App *a)
{
    if (a->view.len) a->cursor = a->view.len - 1;
}

void app_page(App *a, int delta, int page)
{
    app_move(a, delta * (page > 0 ? page : 1));
}

void app_play_pause(App *a)
{
    if (!a->queue.len) {
        play_row(a, a->cursor);
        return;
    }
    if (!player_has_track(a->player)) {
        queue_play(a, a->queue.pos, false);
        return;
    }
    if (player_is_paused(a->player)) {
        player_play(a->player);
    } else if (player_is_playing(a->player)) {
        player_pause(a->player);
    } else {
        if (player_len_frames(a->player) &&
            player_pos_frames(a->player) >= player_len_frames(a->player))
            player_seek_frames(a->player, 0);
        player_play(a->player);
    }
}

void app_stop(App *a)
{
    player_stop(a->player);
    set_status(a, "stopped");
}

void app_next(App *a)
{
    if (!a->queue.len) {
        play_row(a, a->cursor);
        return;
    }
    if (a->queue.pos + 1 < a->queue.len) {
        if (!queue_play(a, a->queue.pos + 1, false)) return;
        return;
    }
    if (a->repeat == REPEAT_ALL) {
        queue_play(a, 0, false);
        return;
    }
    player_stop(a->player);
    set_status(a, "end of list");
}

void app_prev(App *a)
{
    if (!a->queue.len) {
        play_row(a, a->cursor);
        return;
    }
    if (player_has_track(a->player) && player_hz(a->player) &&
        player_pos_frames(a->player) * 1000 >
        3000ull * (uint64_t)player_hz(a->player)) {
        player_seek_frames(a->player, 0);
        return;
    }
    if (a->queue.pos > 0) {
        queue_play(a, a->queue.pos - 1, false);
        return;
    }
    if (a->repeat == REPEAT_ALL && a->queue.len > 1) {
        queue_play(a, a->queue.len - 1, false);
        return;
    }
    player_seek_frames(a->player, 0);
}

void app_seek(App *a, int64_t delta_ms)
{
    if (!player_has_track(a->player)) return;
    player_seek_ms(a->player, delta_ms);
}

void app_volume_delta(App *a, int delta)
{
    float v = player_volume(a->player) + (float)delta / 100.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    player_set_volume(a->player, v);
    if (v > 0.0f) a->last_volume = v;
    set_status(a, "volume %d%%", (int)(v * 100.0f + 0.5f));
}

void app_mute(App *a)
{
    float v = player_volume(a->player);
    if (v > 0.0f) {
        a->last_volume = v;
        player_set_volume(a->player, 0.0f);
        set_status(a, "muted");
    } else {
        float r = a->last_volume > 0.0f ? a->last_volume : 0.7f;
        player_set_volume(a->player, r);
        set_status(a, "volume %d%%", (int)(r * 100.0f + 0.5f));
    }
}

static void resort(App *a, SortMode mode, bool reverse)
{
    char *cur = NULL;
    char *sel = NULL;
    const QTrack *now = app_now(a);
    if (now) cur = xstrdup(now->path);
    if (a->cursor < a->view.len) sel = xstrdup(a->view.items[a->cursor].path);

    a->sort = mode;
    a->reverse = reverse;
    view_sort(&a->view, a->sort, a->reverse);

    if (sel) {
        int row = view_find_path(&a->view, sel);
        if (row >= 0) a->cursor = (size_t)row;
        else if (a->cursor >= a->view.len && a->view.len)
            a->cursor = a->view.len - 1;
        free(sel);
    }
    if (cur) {
        int row = view_find_path(&a->view, cur);
        if (row >= 0) {
            queue_build(a, (size_t)row, a->shuffle);
            if (a->shuffle) a->queue.pos = 0;
        }
        free(cur);
    }
    update_playing_row(a);
}

static bool current_in_view(const App *a)
{
    const QTrack *now = app_now(a);
    return now && view_find_path(&a->view, now->path) >= 0;
}

void app_cycle_sort(App *a)
{
    bool here = current_in_view(a);
    SortMode next = (SortMode)((a->sort + 1) % (SORT_TRACK + 1));
    resort(a, next, a->reverse);
    set_status(a, "sort: %s%s%s", app_sort_name(a->sort),
               a->reverse ? " (reverse)" : "",
               here ? "" : " (view only)");
}

void app_toggle_reverse(App *a)
{
    bool here = current_in_view(a);
    resort(a, a->sort, !a->reverse);
    set_status(a, "order: %s%s%s", app_sort_name(a->sort),
               a->reverse ? " reversed" : "",
               here ? "" : " (view only)");
}

void app_toggle_shuffle(App *a)
{
    const QTrack *now = app_now(a);
    char *cur = now ? xstrdup(now->path) : NULL;
    bool applied = false;
    bool had_cur = cur != NULL;
    a->shuffle = !a->shuffle;
    if (cur) {
        int row = view_find_path(&a->view, cur);
        if (row >= 0) {
            queue_build(a, (size_t)row, a->shuffle);
            if (!a->shuffle) {
                int p = queue_find_path(&a->queue, cur);
                if (p >= 0) a->queue.pos = (size_t)p;
            }
            applied = true;
        }
        free(cur);
    }
    set_status(a, "shuffle %s%s", a->shuffle ? "on" : "off",
               had_cur && !applied ? " (view only)" : "");
    update_playing_row(a);
}

void app_cycle_repeat(App *a)
{
    a->repeat = (RepeatMode)((a->repeat + 1) % 3);
    set_status(a, "repeat: %s", app_repeat_name(a->repeat));
}

void app_delete(App *a)
{
    if (a->cursor >= a->view.len) return;
    Entry *e = &a->view.items[a->cursor];
    if (e->type != ENT_FILE) {
        set_status(a, "only tracks can be removed");
        return;
    }
    char *path = xstrdup(e->path);
    int qidx = queue_find_path(&a->queue, path);
    bool was_current = qidx >= 0 && (size_t)qidx == a->queue.pos && player_has_track(a->player);
    view_remove(&a->view, a->cursor);
    if (qidx >= 0) queue_remove_at(&a->queue, (size_t)qidx);

    if (was_current) {
        player_close(a->player);
        if (a->queue.len) {
            size_t pos = qidx < (int)a->queue.len ? (size_t)qidx : a->queue.len - 1;
            queue_play(a, pos, true);
        }
    }
    if (a->view.len == 0) a->cursor = 0;
    else if (a->cursor >= a->view.len) a->cursor = a->view.len - 1;
    update_playing_row(a);
    set_status(a, "removed from list: %s", path_basename(path));
    free(path);
}

void app_note(App *a, const char *text)
{
    snprintf(a->status, sizeof(a->status), "%s", text ? text : "");
    a->status_until = now_ms() + 2500;
}

void app_tick(App *a)
{
    if (player_take_ended(a->player)) {
        if (a->repeat == REPEAT_ONE) {
            player_seek_frames(a->player, 0);
            player_play(a->player);
        } else if (a->queue.pos + 1 < a->queue.len) {
            queue_play(a, a->queue.pos + 1, true);
        } else if (a->repeat == REPEAT_ALL && a->queue.len) {
            queue_play(a, 0, true);
        } else {
            player_stop(a->player);
            set_status(a, "end of list");
        }
    }
}

const QTrack *app_now(const App *a)
{
    if (!a->queue.len || a->queue.pos >= a->queue.len) return NULL;
    return &a->queue.items[a->queue.pos];
}

const char *app_status(const App *a)
{
    if (!a->status[0] || now_ms() >= a->status_until) return NULL;
    return a->status;
}

const char *app_sort_name(SortMode mode)
{
    switch (mode) {
    case SORT_FILE: return "file";
    case SORT_ARTIST: return "artist";
    case SORT_TITLE: return "title";
    case SORT_ALBUM: return "album";
    case SORT_TRACK: return "track";
    }
    return "?";
}

const char *app_repeat_name(RepeatMode mode)
{
    switch (mode) {
    case REPEAT_OFF: return "off";
    case REPEAT_ALL: return "all";
    case REPEAT_ONE: return "one";
    }
    return "?";
}
