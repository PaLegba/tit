#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "playlist.h"
#include "tags.h"
#include "themes.h"
#include "util.h"

static void test_themes(void)
{
    assert(theme_count == 19);
    assert(themes[0].transparent);
    assert(strcmp(themes[0].name, "terminal") == 0);
    const Theme *g = theme_by_name("gruvbox");
    assert(g && g->bg == 0x282828 && g->colors[11] == 0xFABD2F);
    const Theme *legba = theme_by_name("legba");
    assert(legba && legba->custom && legba->playing == 0x00FF00);
    assert(theme_by_name("nope") == NULL);
}

static void test_time(void)
{
    char b[32];
    format_time(b, sizeof(b), 0);
    assert(strcmp(b, "0:00") == 0);
    format_time(b, sizeof(b), 61000);
    assert(strcmp(b, "1:01") == 0);
    format_time(b, sizeof(b), 3661000);
    assert(strcmp(b, "1:01:01") == 0);
}

static void test_text(void)
{
    char *s = NULL;
    const unsigned char latin[] = { 'C', 'a', 'f', 0xe9 };
    assert(id3_decode_text(0, latin, sizeof(latin), TAG_LATIN1, &s) == 0);
    assert(strcmp(s, "Caf\xc3\xa9") == 0);
    free(s);

    const unsigned char cp[] = { 0xd2, 0xe5, 0xf1, 0xf2 };
    assert(id3_decode_text(0, cp, sizeof(cp), TAG_CP1251, &s) == 0);
    assert(strcmp(s, "\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82") == 0);
    free(s);

    const unsigned char u16le[] = { 0xff, 0xfe, 0x22, 0x04, 0x35, 0x04,
                                    0x41, 0x04, 0x42, 0x04 };
    assert(id3_decode_text(1, u16le, sizeof(u16le), TAG_LATIN1, &s) == 0);
    assert(strcmp(s, "\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82") == 0);
    free(s);

    const unsigned char u16be[] = { 0x04, 0x22, 0x04, 0x35, 0x04, 0x41,
                                    0x04, 0x42 };
    assert(id3_decode_text(2, u16be, sizeof(u16be), TAG_LATIN1, &s) == 0);
    assert(strcmp(s, "\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82") == 0);
    free(s);

    const unsigned char utf8[] = { 0xd0, 0xa2, 0xd0, 0xb5, 0xd1, 0x81,
                                   0xd1, 0x82 };
    assert(id3_decode_text(3, utf8, sizeof(utf8), TAG_LATIN1, &s) == 0);
    assert(strcmp(s, "\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82") == 0);
    free(s);

    assert(id3_decode_text(0, utf8, sizeof(utf8), TAG_UTF8, &s) == 0);
    assert(strcmp(s, "\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82") == 0);
    free(s);

    assert(id3_decode_text(9, utf8, sizeof(utf8), TAG_LATIN1, &s) == -1);
}

static void test_utf8(void)
{
    assert(utf8_casecmp("\xd1\x91\xd0\xbb\xd0\xba\xd0\xb0",
                        "\xd0\x81\xd0\xbb\xd0\xba\xd0\xb0") == 0);
    assert(utf8_casecmp("a", "\xd0\xb1") < 0);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void write_bytes(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(data, 1, n, f) == n);
    fclose(f);
}

static void put_le16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}


static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void put_syncsafe(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)((v >> 21) & 0x7f);
    p[1] = (unsigned char)((v >> 14) & 0x7f);
    p[2] = (unsigned char)((v >> 7) & 0x7f);
    p[3] = (unsigned char)(v & 0x7f);
}

static void test_tags_file(void)
{
    char dir[] = "/tmp/tit-tags-XXXXXX";
    assert(mkdtemp(dir));
    char path[512];
    snprintf(path, sizeof(path), "%s/tagged.mp3", dir);

    unsigned char buf[1024];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "ID3", 3);
    buf[3] = 3;
    buf[4] = 0;
    buf[5] = 0;
    size_t off = 10;
    struct { const char *id; const char *val; } frames[] = {
        { "TIT2", "Title" },
        { "TPE1", "Artist" },
        { "TALB", "Album" },
        { "TRCK", "7/12" },
    };
    for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        size_t n = strlen(frames[i].val);
        memcpy(buf + off, frames[i].id, 4);
        put_be32(buf + off + 4, (uint32_t)(n + 1));
        buf[off + 8] = 0;
        buf[off + 9] = 0;
        buf[off + 10] = 0;
        memcpy(buf + off + 11, frames[i].val, n);
        off += 10 + n + 1;
    }
    put_syncsafe(buf + 6, (uint32_t)(off - 10));

    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(buf, 1, off, f);
    fclose(f);

    TrackMeta m;
    assert(tags_read(path, &m, TAG_LATIN1) == 0);
    assert(m.title && strcmp(m.title, "Title") == 0);
    assert(m.artist && strcmp(m.artist, "Artist") == 0);
    assert(m.album && strcmp(m.album, "Album") == 0);
    assert(m.track == 7);
    meta_free(&m);
    unlink(path);

    snprintf(path, sizeof(path), "%s/v1.mp3", dir);
    unsigned char v1[300];
    memset(v1, 0, sizeof(v1));
    memcpy(v1 + sizeof(v1) - 128, "TAG", 3);
    const char *title = "\xd2\xe5\xf1\xf2";
    memcpy(v1 + sizeof(v1) - 128 + 3, title, 4);
    f = fopen(path, "wb");
    assert(f);
    fwrite(v1, 1, sizeof(v1), f);
    fclose(f);
    assert(tags_read(path, &m, TAG_CP1251) == 0);
    assert(m.title && strcmp(m.title, "\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82") == 0);
    meta_free(&m);
    unlink(path);
    rmdir(dir);
}

static void test_duration_files(void)
{
    char dir[] = "/tmp/tit-dur-XXXXXX";
    assert(mkdtemp(dir));
    char path[512];
    TrackMeta m;

    size_t wav_len = 12 + 8 + 16 + 8 + 88200;
    unsigned char *wav = calloc(1, wav_len);
    assert(wav);
    memcpy(wav, "RIFF", 4);
    put_le32(wav + 4, (uint32_t)(wav_len - 8));
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    put_le32(wav + 16, 16);
    put_le16(wav + 20, 1);
    put_le16(wav + 22, 1);
    put_le32(wav + 24, 44100);
    put_le32(wav + 28, 88200);
    put_le16(wav + 32, 2);
    put_le16(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    put_le32(wav + 40, 88200);
    snprintf(path, sizeof(path), "%s/tone.wav", dir);
    write_bytes(path, wav, wav_len);
    free(wav);
    assert(tags_read(path, &m, TAG_LATIN1) == 0);
    assert(m.duration_ms == 1000);
    meta_free(&m);

    unsigned char flac[4 + 4 + 34];
    memset(flac, 0, sizeof(flac));
    memcpy(flac, "fLaC", 4);
    flac[4] = 0x80;
    flac[5] = 0;
    flac[6] = 0;
    flac[7] = 34;
    uint32_t rate = 44100;
    flac[8 + 10] = (unsigned char)((rate >> 12) & 0xff);
    flac[8 + 11] = (unsigned char)((rate >> 4) & 0xff);
    flac[8 + 12] = (unsigned char)(((rate & 0x0f) << 4) | (1 << 1));
    uint64_t samples = 44100;
    flac[8 + 13] = 0;
    flac[8 + 14] = (unsigned char)(samples >> 24);
    flac[8 + 15] = (unsigned char)(samples >> 16);
    flac[8 + 16] = (unsigned char)(samples >> 8);
    flac[8 + 17] = (unsigned char)samples;
    snprintf(path, sizeof(path), "%s/tone.flac", dir);
    write_bytes(path, flac, sizeof(flac));
    assert(tags_read(path, &m, TAG_LATIN1) == 0);
    assert(m.duration_ms == 1000);
    meta_free(&m);

    unsigned char m4a[36];
    memset(m4a, 0, sizeof(m4a));
    put_be32(m4a, 36);
    memcpy(m4a + 4, "moov", 4);
    put_be32(m4a + 8, 28);
    memcpy(m4a + 12, "mvhd", 4);
    put_be32(m4a + 28, 1000);
    put_be32(m4a + 32, 2000);
    snprintf(path, sizeof(path), "%s/tone.m4a", dir);
    write_bytes(path, m4a, sizeof(m4a));
    assert(tags_read(path, &m, TAG_LATIN1) == 0);
    assert(m.duration_ms == 2000);
    meta_free(&m);

    unsigned char aac[14];
    for (int i = 0; i < 2; i++) {
        unsigned char *f = aac + i * 7;
        f[0] = 0xff;
        f[1] = 0xf1;
        f[2] = 0x50;
        f[3] = 0x80;
        f[4] = 0x00;
        f[5] = 0xe0;
        f[6] = 0x00;
    }
    snprintf(path, sizeof(path), "%s/tone.aac", dir);
    write_bytes(path, aac, sizeof(aac));
    assert(tags_read(path, &m, TAG_LATIN1) == 0);
    assert(m.duration_ms == 46);
    meta_free(&m);

    snprintf(path, sizeof(path), "%s/tone.wav", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/tone.flac", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/tone.m4a", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/tone.aac", dir);
    unlink(path);
    rmdir(dir);
}

static void test_names(void)
{
    assert(is_audio_name("a.mp3"));
    assert(is_audio_name("A.WAV"));
    assert(is_audio_name("a.flac"));
    assert(is_audio_name("a.ogg"));
    assert(is_audio_name("a.oga"));
    assert(is_audio_name("a.aac"));
    assert(is_audio_name("a.m4a"));
    assert(is_audio_name("A.M4A"));
    assert(!is_audio_name("a.opus"));
    assert(!is_audio_name("a.txt"));
    assert(!is_audio_name("mp3"));
    assert(is_playlist_name("x.m3u8"));
}

static void test_playlist(void)
{
    char dir[] = "/tmp/tit-test-XXXXXX";
    assert(mkdtemp(dir));
    char path[512];
    snprintf(path, sizeof(path), "%s/a.mp3", dir);
    write_file(path, "");
    snprintf(path, sizeof(path), "%s/b.mp3", dir);
    write_file(path, "");
    snprintf(path, sizeof(path), "%s/extra.flac", dir);
    write_file(path, "");
    snprintf(path, sizeof(path), "%s/list.m3u8", dir);
    write_file(path,
               "\xef\xbb\xbf#EXTM3U\n"
               "#EXTINF:2,One\n"
               "a.mp3\n"
               "#EXTINF:3,Missing\n"
               "missing.mp3\n"
               "http://example.invalid/x.mp3\n"
               "extra.flac\n");

    View v;
    view_init(&v);
    assert(view_load_playlist(&v, path, TAG_LATIN1) == 0);
    assert(v.len == 4);
    assert(v.items[0].type == ENT_FILE);
    assert(!v.items[0].missing);
    assert(v.items[0].meta.duration_ms == 2000);
    assert(v.items[1].type == ENT_FILE);
    assert(v.items[1].missing);
    assert(strcmp(v.items[1].meta.title, "Missing") == 0);
    assert(v.items[2].type == ENT_URL);
    assert(!view_entry_playable(&v.items[2]));
    assert(v.items[3].type == ENT_FILE);
    assert(!v.items[3].missing);
    assert(view_count_tracks(&v) == 2);

    view_sort(&v, SORT_TITLE, false);
    assert(view_find_path(&v, v.items[0].path) == 0);
    view_remove(&v, 0);
    assert(v.len == 3);
    view_free(&v);

    snprintf(path, sizeof(path), "%s/sub", dir);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/sub/c.mp3", dir);
    write_file(path, "");
    snprintf(path, sizeof(path), "%s/pkg.pls", dir);
    write_file(path, "[playlist]\nFile1=a.mp3\nFile2=missing.mp3\n");

    assert(view_load_dir(&v, dir, TAG_LATIN1) == 0);
    assert(v.len >= 5);
    assert(v.items[0].type == ENT_PARENT);
    assert(strcmp(v.items[0].name, "..") == 0);
    assert(v.items[1].type == ENT_DIR);
    assert(view_count_tracks(&v) == 3);

    snprintf(path, sizeof(path), "%s/pkg.pls", dir);
    assert(view_load_playlist(&v, path, TAG_LATIN1) == 0);
    assert(v.len == 2);
    assert(!v.items[0].missing);
    assert(v.items[1].missing);
    view_free(&v);

    snprintf(path, sizeof(path), "%s/a.mp3", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/b.mp3", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/extra.flac", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/list.m3u8", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/pkg.pls", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/sub/c.mp3", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/sub", dir);
    rmdir(path);
    rmdir(dir);
}

int main(void)
{
    setlocale(LC_CTYPE, "");
    test_themes();
    test_time();
    test_text();
    test_utf8();
    test_tags_file();
    test_duration_files();
    test_names();
    test_playlist();
    puts("tests: ok");
    return 0;
}
