#include "playlist.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "util.h"

void view_init(View *v)
{
    memset(v, 0, sizeof(*v));
}

void view_free(View *v)
{
    for (size_t i = 0; i < v->len; i++) {
        free(v->items[i].path);
        free(v->items[i].name);
        meta_free(&v->items[i].meta);
    }
    free(v->items);
    free(v->source);
    view_init(v);
}

static Entry *view_add(View *v, const char *path, const char *name,
                       EntryType type, TagEncoding enc)
{
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->items = xrealloc(v->items, v->cap * sizeof(*v->items));
    }
    Entry *e = &v->items[v->len];
    memset(e, 0, sizeof(*e));
    e->path = xstrdup(path);
    e->name = xstrdup(name);
    e->type = type;
    e->seq = (uint32_t)v->len;
    if (type == ENT_FILE) tags_read(path, &e->meta, enc);
    v->len++;
    return e;
}

static bool hidden_name(const char *name)
{
    return name[0] == '.' && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static int cmp_initial(const void *pa, const void *pb)
{
    const Entry *a = pa, *b = pb;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    return utf8_casecmp(a->name, b->name);
}

static void set_seq(View *v)
{
    for (size_t i = 0; i < v->len; i++) v->items[i].seq = (uint32_t)i;
}

int view_load_dir(View *v, const char *path, TagEncoding enc)
{
    char *source = path_absolute(path);
    DIR *d = opendir(source);
    if (!d) {
        free(source);
        return -1;
    }
    view_free(v);
    v->source = source;
    v->is_playlist = false;

    char *parent = path_parent(source);
    if (strcmp(parent, source) != 0)
        view_add(v, parent, "..", ENT_PARENT, enc);
    free(parent);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (hidden_name(de->d_name)) continue;
        char *full = path_join(source, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                view_add(v, full, de->d_name, ENT_DIR, enc);
            } else if (S_ISREG(st.st_mode)) {
                if (is_mp3_name(de->d_name))
                    view_add(v, full, de->d_name, ENT_FILE, enc);
                else if (is_playlist_name(de->d_name))
                    view_add(v, full, de->d_name, ENT_PLAYLIST, enc);
            }
        }
        free(full);
    }
    closedir(d);
    qsort(v->items, v->len, sizeof(*v->items), cmp_initial);
    set_seq(v);
    return 0;
}

static void resolve_entry(View *v, const char *base, const char *line,
                          TagEncoding enc, const char *ext_title, int ext_ms)
{
    if (is_url(line)) {
        Entry *e = view_add(v, line, line, ENT_URL, enc);
        e->meta.duration_ms = ext_ms;
        if (ext_title && !e->meta.title) e->meta.title = xstrdup(ext_title);
        return;
    }
    char *path = line[0] == '/' ? xstrdup(line) : path_join(base, line);
    struct stat st;
    bool exists = stat(path, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        view_add(v, path, path_basename(path), ENT_DIR, enc);
    } else if (is_playlist_name(path)) {
        if (exists) view_add(v, path, path_basename(path), ENT_PLAYLIST, enc);
    } else if (is_mp3_name(path)) {
        Entry *e = view_add(v, path, path_basename(path), ENT_FILE, enc);
        e->missing = !exists || !S_ISREG(st.st_mode);
        if (e->meta.duration_ms == 0 && ext_ms > 0) e->meta.duration_ms = ext_ms;
        if (!e->meta.title && ext_title) e->meta.title = xstrdup(ext_title);
    }
    free(path);
}

static void load_m3u(View *v, FILE *f, const char *base, TagEncoding enc)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    char *ext_title = NULL;
    int ext_ms = 0;
    bool first = true;
    while ((n = getline(&line, &cap, f)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (first) {
            if ((unsigned char)line[0] == 0xef &&
                (unsigned char)line[1] == 0xbb &&
                (unsigned char)line[2] == 0xbf)
                memmove(line, line + 3, strlen(line + 3) + 1);
            first = false;
        }
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) continue;
        if (strncmp(s, "#EXTINF:", 8) == 0) {
            char *comma = strchr(s + 8, ',');
            if (comma) {
                ext_ms = atoi(s + 8) * 1000;
                free(ext_title);
                ext_title = xstrdup(comma + 1);
            }
            continue;
        }
        if (*s == '#') continue;
        resolve_entry(v, base, s, enc, ext_title, ext_ms);
        free(ext_title);
        ext_title = NULL;
        ext_ms = 0;
    }
    free(ext_title);
    free(line);
}

static void load_pls(View *v, FILE *f, const char *base, TagEncoding enc)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        if (strncasecmp(line, "File", 4) != 0) continue;
        char *s = eq + 1;
        while (*s == ' ' || *s == '\t') s++;
        if (*s) resolve_entry(v, base, s, enc, NULL, 0);
    }
    free(line);
}

int view_load_playlist(View *v, const char *path, TagEncoding enc)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *source = path_absolute(path);
    char *base = path_dirname(source);
    size_t plen = strlen(path);
    bool pls = is_playlist_name(path) && plen >= 4 &&
               strcasecmp(path + plen - 4, ".pls") == 0;
    view_free(v);
    v->source = source;
    v->is_playlist = true;

    if (pls)
        load_pls(v, f, base, enc);
    else
        load_m3u(v, f, base, enc);

    fclose(f);
    free(base);
    return 0;
}

int view_load_files(View *v, char *const *paths, size_t n, TagEncoding enc)
{
    view_free(v);
    v->source = xstrdup("selection");
    v->is_playlist = true;
    v->is_selection = true;
    for (size_t i = 0; i < n; i++) {
        if (is_url(paths[i])) {
            view_add(v, paths[i], paths[i], ENT_URL, enc);
            continue;
        }
        struct stat st;
        if (stat(paths[i], &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        char *abs = path_absolute(paths[i]);
        view_add(v, abs, path_basename(abs), ENT_FILE, enc);
        free(abs);
    }
    return v->len ? 0 : -1;
}

static SortMode g_sort_mode;
static bool g_sort_reverse;

static int entry_rank(EntryType t)
{
    switch (t) {
    case ENT_PARENT: return 0;
    case ENT_DIR: return 1;
    case ENT_PLAYLIST: return 2;
    case ENT_FILE: return 3;
    case ENT_URL: return 4;
    }
    return 5;
}

static const char *key_artist(const Entry *e)
{
    return e->meta.artist && *e->meta.artist ? e->meta.artist
        : (e->meta.title && *e->meta.title ? e->meta.title : e->name);
}

static const char *key_title(const Entry *e)
{
    return e->meta.title && *e->meta.title ? e->meta.title : e->name;
}

static int cmp_view(const void *pa, const void *pb)
{
    const Entry *a = pa, *b = pb;
    if (a->type == ENT_PARENT || b->type == ENT_PARENT) {
        if (a->type == b->type) return 0;
        return a->type == ENT_PARENT ? -1 : 1;
    }
    int r = 0;
    if (g_sort_mode == SORT_FILE) {
        r = a->seq < b->seq ? -1 : (a->seq > b->seq ? 1 : 0);
    } else {
        int ra = entry_rank(a->type), rb = entry_rank(b->type);
        if (ra != rb) return ra < rb ? -1 : 1;
        switch (g_sort_mode) {
        case SORT_ARTIST:
            r = utf8_casecmp(key_artist(a), key_artist(b));
            if (!r) r = utf8_casecmp(key_title(a), key_title(b));
            break;
        case SORT_TITLE:
            r = utf8_casecmp(key_title(a), key_title(b));
            if (!r) r = utf8_casecmp(key_artist(a), key_artist(b));
            break;
        case SORT_ALBUM:
            r = utf8_casecmp(a->meta.album ? a->meta.album : "",
                             b->meta.album ? b->meta.album : "");
            if (!r) r = a->meta.track - b->meta.track;
            if (!r) r = utf8_casecmp(key_title(a), key_title(b));
            break;
        case SORT_TRACK: {
            int ta = a->meta.track ? a->meta.track : INT_MAX;
            int tb = b->meta.track ? b->meta.track : INT_MAX;
            r = ta < tb ? -1 : (ta > tb ? 1 : 0);
            if (!r) r = utf8_casecmp(a->meta.album ? a->meta.album : "",
                                     b->meta.album ? b->meta.album : "");
            if (!r) r = utf8_casecmp(key_title(a), key_title(b));
            break;
        }
        default:
            break;
        }
    }
    if (r && g_sort_reverse) r = -r;
    if (!r) {
        r = utf8_casecmp(a->name, b->name);
        if (r && g_sort_reverse) r = -r;
    }
    return r;
}

void view_sort(View *v, SortMode mode, bool reverse)
{
    if (v->len < 2) return;
    g_sort_mode = mode;
    g_sort_reverse = reverse;
    qsort(v->items, v->len, sizeof(*v->items), cmp_view);
}

bool view_entry_playable(const Entry *e)
{
    return e->type == ENT_FILE && !e->missing;
}

size_t view_count_tracks(const View *v)
{
    size_t n = 0;
    for (size_t i = 0; i < v->len; i++)
        if (view_entry_playable(&v->items[i])) n++;
    return n;
}

int view_find_path(const View *v, const char *path)
{
    if (!path) return -1;
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(v->items[i].path, path) == 0) return (int)i;
    return -1;
}

void view_remove(View *v, size_t index)
{
    if (index >= v->len) return;
    Entry *e = &v->items[index];
    free(e->path);
    free(e->name);
    meta_free(&e->meta);
    memmove(e, e + 1, (v->len - index - 1) * sizeof(*e));
    v->len--;
}

const char *view_title(const View *v)
{
    if (v->is_selection) return "selection";
    return path_basename(v->source ? v->source : "");
}
