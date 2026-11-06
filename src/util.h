#ifndef TIT_UTIL_H
#define TIT_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

const char *path_basename(const char *path);
char *path_dirname(const char *path);
char *path_join(const char *a, const char *b);
char *path_absolute(const char *path);
char *path_parent(const char *path);

bool is_url(const char *s);
bool is_mp3_name(const char *name);
bool is_playlist_name(const char *name);

size_t utf8_next(const char *s, uint32_t *cp);
int utf8_casecmp(const char *a, const char *b);

void format_time(char *buf, size_t size, uint64_t ms);
uint64_t now_ms(void);

#endif
