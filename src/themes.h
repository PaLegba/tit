#ifndef TIT_THEMES_H
#define TIT_THEMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    uint32_t colors[16];
    uint32_t fg;
    uint32_t bg;
    bool transparent;
    bool custom;
    uint32_t accent;
    uint32_t playing;
    uint32_t dir;
    uint32_t playlist;
    uint32_t dim;
    uint32_t error;
    uint32_t border;
} Theme;

extern const Theme themes[];
extern const size_t theme_count;

const Theme *theme_by_name(const char *name);

#endif
