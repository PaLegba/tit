#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("tit: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) {
        fputs("tit: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("tit: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *p = xmalloc(len + 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (n < 0) {
        va_end(ap);
        return xstrdup("");
    }
    char *p = xmalloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

const char *path_basename(const char *path)
{
    if (!path || !*path) return ".";
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    size_t i = n;
    while (i > 0 && path[i - 1] != '/') i--;
    if (i == n) return "/";
    return path + i;
}

char *path_dirname(const char *path)
{
    if (!path || !*path) return xstrdup(".");
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    size_t i = n;
    while (i > 0 && path[i - 1] != '/') i--;
    if (i == 0) return xstrdup(".");
    while (i > 1 && path[i - 1] == '/') i--;
    if (i == 0) return xstrdup("/");
    return xstrndup(path, i);
}

char *path_join(const char *a, const char *b)
{
    if (!a || !*a) return xstrdup(b ? b : "");
    if (!b || !*b) return xstrdup(a);
    if (b[0] == '/') return xstrdup(b);
    size_t na = strlen(a);
    bool slash = a[na - 1] == '/';
    return xasprintf(slash ? "%s%s" : "%s/%s", a, b);
}

char *path_absolute(const char *path)
{
    char *r = realpath(path, NULL);
    if (r) return r;
    return xstrdup(path ? path : "");
}

char *path_parent(const char *path)
{
    char *d = path_dirname(path);
    char *r = path_absolute(d);
    free(d);
    return r;
}

bool is_url(const char *s)
{
    return s && (strncmp(s, "http://", 7) == 0 ||
                 strncmp(s, "https://", 8) == 0 ||
                 strncmp(s, "ftp://", 6) == 0);
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ns = strlen(s), nx = strlen(suffix);
    if (ns < nx) return false;
    return strcasecmp(s + ns - nx, suffix) == 0;
}

bool is_mp3_name(const char *name)
{
    return name && ends_with_ci(name, ".mp3");
}

bool is_playlist_name(const char *name)
{
    return name && (ends_with_ci(name, ".m3u") ||
                    ends_with_ci(name, ".m3u8") ||
                    ends_with_ci(name, ".pls") ||
                    ends_with_ci(name, ".lst") ||
                    ends_with_ci(name, ".list"));
}

size_t utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t c;
    int n;

    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xe0) == 0xc0) {
        n = 2;
        c = p[0] & 0x1f;
    } else if ((p[0] & 0xf0) == 0xe0) {
        n = 3;
        c = p[0] & 0x0f;
    } else if ((p[0] & 0xf8) == 0xf0) {
        n = 4;
        c = p[0] & 0x07;
    } else {
        *cp = 0xfffd;
        return 1;
    }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            *cp = 0xfffd;
            return 1;
        }
        c = (c << 6) | (p[i] & 0x3f);
    }
    if ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) ||
        (n == 4 && c < 0x10000) || c > 0x10ffff ||
        (c >= 0xd800 && c <= 0xdfff)) {
        *cp = 0xfffd;
        return 1;
    }
    *cp = c;
    return (size_t)n;
}

static uint32_t cp_lower(uint32_t cp)
{
    /* Locale-independent simple case folding for the common ranges. */
    if (cp >= 'A' && cp <= 'Z') return cp + 0x20;
    if (cp >= 0x00c0 && cp <= 0x00de && cp != 0x00d7) return cp + 0x20;
    if (cp == 0x0401) return 0x0451;
    if (cp >= 0x0410 && cp <= 0x042f) return cp + 0x20;
    if (cp >= 0x0391 && cp <= 0x03a9 && cp != 0x03a2) return cp + 0x20;
    return cp;
}

int utf8_casecmp(const char *a, const char *b)
{
    while (*a || *b) {
        uint32_t ca = 0, cb = 0;
        if (*a) a += utf8_next(a, &ca);
        if (*b) b += utf8_next(b, &cb);
        if (!ca && !cb) break;
        int la = (int)cp_lower((uint32_t)towlower((wint_t)ca));
        int lb = (int)cp_lower((uint32_t)towlower((wint_t)cb));
        if (la != lb) return la < lb ? -1 : 1;
        if (!ca) return -1;
        if (!cb) return 1;
    }
    return 0;
}

void format_time(char *buf, size_t size, uint64_t ms)
{
    uint64_t total = ms / 1000;
    uint64_t h = total / 3600;
    uint64_t m = (total / 60) % 60;
    uint64_t s = total % 60;
    if (h)
        snprintf(buf, size, "%llu:%02llu:%02llu",
                 (unsigned long long)h, (unsigned long long)m,
                 (unsigned long long)s);
    else
        snprintf(buf, size, "%llu:%02llu",
                 (unsigned long long)m, (unsigned long long)s);
}

uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    return (uint64_t)time(NULL) * 1000;
}
