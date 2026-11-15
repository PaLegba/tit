#include "mod.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define POCKETMOD_IMPLEMENTATION
#include "pocketmod.h"

#define MOD_DURATION_RATE 8000

static uint64_t render_frames(const void *data, size_t size, int rate)
{
    if (!data || size < 600 || size > INT_MAX || rate <= 0) return 0;

    pocketmod_context c;
    if (!pocketmod_init(&c, data, (int)size, rate)) return 0;

    float buf[4096 * 2];
    uint64_t frames = 0;
    while (pocketmod_loop_count(&c) == 0) {
        int got = pocketmod_render(&c, buf, (int)sizeof(buf));
        int n = got / (int)(sizeof(float) * 2);
        if (n <= 0) break;
        frames += (uint64_t)n;
        /* shortcut: reject a module longer than 24 hours */
        if (frames > (uint64_t)rate * 60 * 60 * 24) break;
    }
    return frames;
}

uint64_t mod_duration_frames(const void *data, size_t size, int rate)
{
    uint64_t frames = render_frames(data, size, MOD_DURATION_RATE);
    return frames && rate > 0
        ? frames * (uint64_t)rate / MOD_DURATION_RATE : 0;
}

int mod_duration_ms(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 600 ||
        st.st_size > INT_MAX) {
        close(fd);
        return 0;
    }

    size_t size = (size_t)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return 0;

    uint64_t frames = render_frames(map, size, MOD_DURATION_RATE);
    munmap(map, size);
    if (!frames) return 0;

    uint64_t ms = frames * 1000 / MOD_DURATION_RATE;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}
