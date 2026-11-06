#ifndef TIT_TAGS_H
#define TIT_TAGS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TAG_LATIN1 = 0,
    TAG_CP1251 = 1,
    TAG_UTF8 = 2
} TagEncoding;

typedef struct {
    char *title;
    char *artist;
    char *album;
    int track;
    int duration_ms;
    bool has_tag;
} TrackMeta;

void meta_init(TrackMeta *m);
void meta_free(TrackMeta *m);
void meta_copy(TrackMeta *dst, const TrackMeta *src);

/* Returns 0 if the file could be opened, -1 otherwise. Missing tags are fine. */
int tags_read(const char *path, TrackMeta *m, TagEncoding enc);

/* Decode an ID3 text frame body. Exposed for tests. */
int id3_decode_text(unsigned char encoding, const unsigned char *data,
                    size_t len, TagEncoding fallback, char **out);

#endif
