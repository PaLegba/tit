#include "tags.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

#define MP3_SCAN_MAX (128u * 1024u)

static int read_at(int fd, off_t off, void *buf, size_t n);

void meta_init(TrackMeta *m)
{
    memset(m, 0, sizeof(*m));
}

void meta_free(TrackMeta *m)
{
    free(m->title);
    free(m->artist);
    free(m->album);
    meta_init(m);
}

void meta_copy(TrackMeta *dst, const TrackMeta *src)
{
    meta_init(dst);
    dst->title = src->title ? xstrdup(src->title) : NULL;
    dst->artist = src->artist ? xstrdup(src->artist) : NULL;
    dst->album = src->album ? xstrdup(src->album) : NULL;
    dst->track = src->track;
    dst->duration_ms = src->duration_ms;
    dst->has_tag = src->has_tag;
}

static void put_utf8(char *out, size_t *pos, uint32_t cp)
{
    if (cp < 0x80) {
        out[(*pos)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*pos)++] = (char)(0xc0 | (cp >> 6));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out[(*pos)++] = (char)(0xe0 | (cp >> 12));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3f));
    } else {
        out[(*pos)++] = (char)(0xf0 | (cp >> 18));
        out[(*pos)++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3f));
    }
}

static const uint16_t cp1251_high[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

static uint16_t cp1251_cp(uint8_t b)
{
    if (b < 0x80) return b;
    return cp1251_high[b - 0x80];
}

static void decode_8bit(const unsigned char *data, size_t len, TagEncoding enc,
                        char **out)
{
    if (enc == TAG_UTF8) {
        size_t n = 0;
        while (n < len && data[n]) n++;
        *out = xstrndup((const char *)data, n);
        return;
    }
    char *s = xmalloc(len * 3 + 1);
    size_t o = 0;
    for (size_t i = 0; i < len && data[i]; i++) {
        uint32_t cp = enc == TAG_CP1251 ? cp1251_cp(data[i]) : data[i];
        put_utf8(s, &o, cp);
    }
    s[o] = '\0';
    *out = s;
}

static void decode_utf16(const unsigned char *data, size_t len, bool big_endian,
                         char **out)
{
    char *s = xmalloc(len * 2 + 1);
    size_t o = 0;
    size_t i = 0;
    if (len >= 2) {
        if (data[0] == 0xff && data[1] == 0xfe) {
            big_endian = false;
            i = 2;
        } else if (data[0] == 0xfe && data[1] == 0xff) {
            big_endian = true;
            i = 2;
        }
    }
    while (i + 1 < len) {
        uint32_t cp = big_endian
            ? ((uint32_t)data[i] << 8 | data[i + 1])
            : ((uint32_t)data[i + 1] << 8 | data[i]);
        i += 2;
        if (cp == 0) break;
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < len) {
            uint32_t lo = big_endian
                ? ((uint32_t)data[i] << 8 | data[i + 1])
                : ((uint32_t)data[i + 1] << 8 | data[i]);
            if (lo >= 0xdc00 && lo <= 0xdfff) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                i += 2;
            } else {
                cp = 0xfffd;
            }
        } else if (cp >= 0xd800 && cp <= 0xdfff) {
            cp = 0xfffd;
        }
        put_utf8(s, &o, cp);
    }
    s[o] = '\0';
    *out = s;
}

int id3_decode_text(unsigned char encoding, const unsigned char *data,
                    size_t len, TagEncoding fallback, char **out)
{
    *out = NULL;
    if (encoding == 0) {
        decode_8bit(data, len, fallback, out);
    } else if (encoding == 1) {
        decode_utf16(data, len, false, out);
    } else if (encoding == 2) {
        decode_utf16(data, len, true, out);
    } else if (encoding == 3) {
        char *s = xmalloc(len + 1);
        size_t n = 0;
        while (n < len && data[n]) n++;
        memcpy(s, data, n);
        s[n] = '\0';
        *out = s;
    } else {
        return -1;
    }
    return 0;
}

static void trim_trailing(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

static void set_text(char **dst, const unsigned char *data, size_t len,
                     TagEncoding enc)
{
    if (!data || len == 0) return;
    unsigned char encoding = data[0];
    char *s = NULL;
    if (id3_decode_text(encoding, data + 1, len - 1, enc, &s) != 0) return;
    trim_trailing(s);
    if (!*s) {
        free(s);
        return;
    }
    if (!*dst) {
        *dst = s;
    } else {
        free(s);
    }
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t syncsafe32(const unsigned char *p)
{
    return ((uint32_t)(p[0] & 0x7f) << 21) |
           ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) |
           (uint32_t)(p[3] & 0x7f);
}

static size_t unsync(unsigned char *buf, size_t len)
{
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        buf[w++] = buf[i];
        if (buf[i] == 0xff && i + 1 < len && buf[i + 1] == 0x00)
            i++;
    }
    return w;
}

static void frame_text(TrackMeta *m, const char *id,
                       const unsigned char *data, size_t len, TagEncoding enc)
{
    if (strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0)
        set_text(&m->title, data, len, enc);
    else if (strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0)
        set_text(&m->artist, data, len, enc);
    else if (strcmp(id, "TALB") == 0 || strcmp(id, "TAL") == 0)
        set_text(&m->album, data, len, enc);
    else if (strcmp(id, "TRCK") == 0 || strcmp(id, "TRK") == 0) {
        char *s = NULL;
        set_text(&s, data, len, enc);
        if (s) {
            m->track = atoi(s);
            free(s);
        }
    }
}

static void parse_id3v2(unsigned char *buf, size_t len, TrackMeta *m,
                        TagEncoding enc)
{
    if (len < 10 || memcmp(buf, "ID3", 3) != 0) return;
    unsigned version = buf[3];
    unsigned flags = buf[5];
    uint32_t tagsize = syncsafe32(buf + 6);
    size_t total = 10u + tagsize;
    if (total > len) total = len;

    unsigned char *p = buf + 10;
    size_t avail = total > 10 ? total - 10 : 0;
    if (version == 4 && (flags & 0x10) && avail >= 10) avail -= 10; /* footer */

    if (flags & 0x80) avail = unsync(p, avail);

    size_t off = 0;
    if (version >= 3 && (flags & 0x40)) {
        if (avail < 4) return;
        size_t ext = version == 4 ? syncsafe32(p) : be32(p) + 4;
        if (ext > avail) return;
        off = ext;
    } else if (version == 2 && (flags & 0x40)) {
        if (avail < 4) return;
        size_t ext = 3u + ((size_t)p[0] << 16 | p[1] << 8 | p[2]);
        if (ext > avail) return;
        off = ext;
    }

    while (off + (version == 2 ? 6u : 10u) <= avail) {
        const unsigned char *fh = p + off;
        char id[5] = {0};
        size_t fsize;
        size_t hdr = version == 2 ? 6 : 10;
        if (fh[0] == 0) break;
        if (version == 2) {
            memcpy(id, fh, 3);
            fsize = (size_t)fh[3] << 16 | (size_t)fh[4] << 8 | fh[5];
        } else {
            memcpy(id, fh, 4);
            fsize = version == 4 ? syncsafe32(fh + 4) : be32(fh + 4);
            unsigned fflags = (unsigned)fh[8] << 8 | fh[9];
            if (version == 3) {
                if (fflags & 0xc0) { off += hdr + fsize; continue; }
            } else {
                if (fflags & 0x0c) { off += hdr + fsize; continue; }
            }
        }
        if (fsize > avail - off - hdr) break;
        const unsigned char *data = fh + hdr;
        size_t dlen = fsize;
        if (version == 4 && ((fh[8] << 8 | fh[9]) & 0x01)) {
            if (dlen >= 4) { data += 4; dlen -= 4; }
        }
        if (version == 4 && ((fh[8] << 8 | fh[9]) & 0x02)) {
            unsigned char *tmp = xmalloc(dlen ? dlen : 1);
            memcpy(tmp, data, dlen);
            dlen = unsync(tmp, dlen);
            frame_text(m, id, tmp, dlen, enc);
            free(tmp);
        } else {
            frame_text(m, id, data, dlen, enc);
        }
        off += hdr + fsize;
    }
}

static bool is_text_frame(const char *id)
{
    return strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0 ||
           strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0 ||
           strcmp(id, "TALB") == 0 || strcmp(id, "TAL") == 0 ||
           strcmp(id, "TRCK") == 0 || strcmp(id, "TRK") == 0;
}

/* Parse frames without reading large APIC/cover-art payloads into memory. */
static void parse_id3v2_fd(int fd, uint64_t tag_end, unsigned version,
                           unsigned flags, TrackMeta *m, TagEncoding enc)
{
    uint64_t pos = 10;
    if (flags & 0x40) {
        unsigned char ext[4];
        if (read_at(fd, pos, ext, 4) == 4) {
            uint32_t sz;
            if (version == 4) sz = syncsafe32(ext);
            else if (version == 3) sz = be32(ext) + 4;
            else sz = 6;
            if (sz <= tag_end - pos) pos += sz;
        }
    }
    size_t hdr = version == 2 ? 6 : 10;
    while (pos + hdr <= tag_end) {
        unsigned char fh[10];
        if (read_at(fd, pos, fh, hdr) != (int)hdr) break;
        if (fh[0] == 0) break;
        char id[5] = {0};
        size_t fsize;
        unsigned fflags = 0;
        if (version == 2) {
            memcpy(id, fh, 3);
            fsize = (size_t)fh[3] << 16 | (size_t)fh[4] << 8 | fh[5];
        } else {
            memcpy(id, fh, 4);
            fsize = version == 4 ? syncsafe32(fh + 4) : be32(fh + 4);
            fflags = (unsigned)fh[8] << 8 | fh[9];
        }
        if (fsize > tag_end - pos - hdr) break;
        bool skip = false;
        if (version == 3)
            skip = (fflags & 0xe0) != 0; /* compression, encryption, grouping */
        else if (version == 4)
            skip = (fflags & 0x4c) != 0; /* grouping, compression, encryption */
        if (!skip && is_text_frame(id)) {
            size_t n = fsize > 4096 ? 4096 : fsize;
            unsigned char *tmp = xmalloc(n ? n : 1);
            if (read_at(fd, pos + hdr, tmp, n) == (int)n) {
                unsigned char *data = tmp;
                size_t dlen = n;
                if (version == 4 && (fflags & 0x01) && dlen >= 4) {
                    data += 4;
                    dlen -= 4;
                }
                if (version == 4 && (fflags & 0x02))
                    dlen = unsync(data, dlen);
                frame_text(m, id, data, dlen, enc);
            }
            free(tmp);
        }
        pos += hdr + fsize;
    }
}

static void parse_id3v1(const unsigned char *t, TrackMeta *m, TagEncoding enc)
{
    if (memcmp(t, "TAG", 3) != 0) return;
    struct { const unsigned char *p; size_t n; char **dst; } fields[] = {
        { t + 3, 30, &m->title },
        { t + 33, 30, &m->artist },
        { t + 63, 30, &m->album },
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (*fields[i].dst) continue;
        size_t n = fields[i].n;
        while (n > 0 && (fields[i].p[n - 1] == 0 || fields[i].p[n - 1] == ' '))
            n--;
        char *s = NULL;
        decode_8bit(fields[i].p, n, enc, &s);
        trim_trailing(s);
        if (*s) *fields[i].dst = s;
        else free(s);
    }
    if (m->track == 0 && t[125] == 0 && t[126] != 0)
        m->track = t[126];
    m->has_tag = true;
}

typedef struct {
    int version;
    int layer;
    int bitrate;
    int hz;
    int channels;
    int samples;
    int frame_bytes;
    int cbr;
    char xing[4];
    uint32_t frames;
} Mp3Info;

static int bitrate_for(int version, int index)
{
    static const int mpeg1_l3[15] =
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
    static const int mpeg2_l3[15] =
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
    if (version == 1) return mpeg1_l3[index];
    return mpeg2_l3[index];
}

static int sample_rate_for(int version, int index)
{
    static const int rates[3][3] = {
        { 44100, 48000, 32000 },
        { 22050, 24000, 16000 },
        { 11025, 12000, 8000 },
    };
    int v = version == 1 ? 0 : (version == 2 ? 1 : 2);
    return rates[v][index];
}

static bool mp3_header(const unsigned char *b, size_t len, Mp3Info *inf)
{
    if (len < 4) return false;
    if (b[0] != 0xff || (b[1] & 0xe0) != 0xe0) return false;
    int version_bits = (b[1] >> 3) & 3;
    int layer_bits = (b[1] >> 1) & 3;
    int br_index = (b[2] >> 4) & 0x0f;
    int sr_index = (b[2] >> 2) & 3;
    int padding = (b[2] >> 1) & 1;
    int mode = (b[3] >> 6) & 3;
    if (version_bits == 1 || layer_bits != 1 || br_index == 0 ||
        br_index == 15 || sr_index == 3)
        return false;
    int version = version_bits == 3 ? 1 : (version_bits == 2 ? 2 : 0);
    int bitrate = bitrate_for(version, br_index);
    int hz = sample_rate_for(version, sr_index);
    if (!bitrate || !hz) return false;
    int samples = version == 1 ? 1152 : 576;
    int frame_bytes = (version == 1 ? 144 : 72) * bitrate * 1000 / hz + padding;
    if (frame_bytes < 4) return false;
    memset(inf, 0, sizeof(*inf));
    inf->version = version;
    inf->layer = 3;
    inf->bitrate = bitrate;
    inf->hz = hz;
    inf->channels = mode == 3 ? 1 : 2;
    inf->samples = samples;
    inf->frame_bytes = frame_bytes;
    inf->cbr = 1;
    return true;
}

static void parse_xing(const unsigned char *frame, size_t len, Mp3Info *inf)
{
    size_t side = inf->version == 1
        ? (inf->channels == 1 ? 17 : 32)
        : (inf->channels == 1 ? 9 : 17);
    size_t off = 4 + side;
    if (off + 12 > len) return;
    if (memcmp(frame + off, "Xing", 4) == 0 ||
        memcmp(frame + off, "Info", 4) == 0) {
        memcpy(inf->xing, frame + off, 4);
        uint32_t flags = be32(frame + off + 4);
        if (flags & 1) inf->frames = be32(frame + off + 8);
        if (inf->frames) inf->cbr = 0;
    } else if (off + 36 + 18 <= len && memcmp(frame + 36, "VBRI", 4) == 0) {
        inf->frames = be32(frame + 36 + 14);
        if (inf->frames) inf->cbr = 0;
    }
}

static bool find_first_frame(const unsigned char *buf, size_t len,
                             size_t *offset, Mp3Info *inf)
{
    for (size_t i = 0; i + 4 <= len; i++) {
        if (mp3_header(buf + i, len - i, inf)) {
            *offset = i;
            parse_xing(buf + i, len - i, inf);
            return true;
        }
    }
    return false;
}

static int read_at(int fd, off_t off, void *buf, size_t n)
{
    unsigned char *p = buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, p + done, n - done, off + (off_t)done);
        if (r <= 0) break;
        done += (size_t)r;
    }
    return (int)done;
}

int tags_read(const char *path, TrackMeta *m, TagEncoding enc)
{
    meta_init(m);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 4) {
        close(fd);
        return 0;
    }
    uint64_t file_size = (uint64_t)st.st_size;

    unsigned char head[10];
    if (read_at(fd, 0, head, 10) < 10) {
        close(fd);
        return 0;
    }

    uint64_t audio_start = 0;
    if (memcmp(head, "ID3", 3) == 0) {
        unsigned version = head[3];
        unsigned flags = head[5];
        uint32_t tagsize = syncsafe32(head + 6);
        audio_start = 10u + tagsize +
            ((version == 4 && (flags & 0x10)) ? 10u : 0u);
        uint64_t tag_end = 10u + tagsize;
        if (tag_end > file_size) tag_end = file_size;
        audio_start = tag_end + ((version == 4 && (flags & 0x10)) ? 10u : 0u);
        if (audio_start > file_size) audio_start = file_size;
        if (flags & 0x80) {
            /* Unsynchronised tags are rare; the whole tag is needed. */
            if (tag_end <= 1024u * 1024u) {
                unsigned char *tag = xmalloc((size_t)tag_end);
                if (read_at(fd, 0, tag, (size_t)tag_end) == (int)tag_end)
                    parse_id3v2(tag, (size_t)tag_end, m, enc);
                free(tag);
            }
        } else if (version >= 2 && version <= 4) {
            parse_id3v2_fd(fd, tag_end, version, flags, m, enc);
        }
    }

    uint64_t audio_end = file_size;
    unsigned char tail[128];
    if (file_size >= 128 && read_at(fd, (off_t)(file_size - 128), tail, 128) == 128) {
        if (memcmp(tail, "TAG", 3) == 0) {
            parse_id3v1(tail, m, enc);
            audio_end -= 128;
        }
    }

    if (audio_end > audio_start) {
        size_t want = (size_t)(audio_end - audio_start);
        if (want > MP3_SCAN_MAX) want = MP3_SCAN_MAX;
        if (want >= 4) {
            unsigned char *buf = xmalloc(want);
            int got = read_at(fd, (off_t)audio_start, buf, want);
            if (got >= 4) {
                size_t off = 0;
                Mp3Info inf;
                if (find_first_frame(buf, (size_t)got, &off, &inf)) {
                    uint64_t bytes = audio_end - audio_start - off;
                    if (inf.frames) {
                        m->duration_ms = (int)((uint64_t)inf.frames * inf.samples
                                               * 1000 / inf.hz);
                    } else if (inf.bitrate) {
                        m->duration_ms = (int)(bytes * 8 / inf.bitrate);
                    }
                }
            }
            free(buf);
        }
    }

    close(fd);
    m->has_tag = m->title || m->artist || m->album;
    return 0;
}
