#ifndef TIT_MOD_H
#define TIT_MOD_H

#include <stddef.h>
#include <stdint.h>

uint64_t mod_duration_frames(const void *data, size_t size, int rate);
int mod_duration_ms(const char *path);

#endif
