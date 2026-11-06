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

static void test_playlist(void)
{
    char dir[] = "/tmp/tit-test-XXXXXX";
    assert(mkdtemp(dir));
    char path[512];
    snprintf(path, sizeof(path), "%s/a.mp3", dir);
    write_file(path, "");
    snprintf(path, sizeof(path), "%s/b.mp3", dir);
    write_file(path, "");
    snprintf(path, sizeof(path), "%s/list.m3u8", dir);
    write_file(path,
               "\xef\xbb\xbf#EXTM3U\n"
               "#EXTINF:2,One\n"
               "a.mp3\n"
               "#EXTINF:3,Missing\n"
               "missing.mp3\n"
               "http://example.invalid/x.mp3\n");

    View v;
    view_init(&v);
    assert(view_load_playlist(&v, path, TAG_LATIN1) == 0);
    assert(v.len == 3);
    assert(v.items[0].type == ENT_FILE);
    assert(!v.items[0].missing);
    assert(v.items[0].meta.duration_ms == 2000);
    assert(v.items[1].type == ENT_FILE);
    assert(v.items[1].missing);
    assert(strcmp(v.items[1].meta.title, "Missing") == 0);
    assert(v.items[2].type == ENT_URL);
    assert(!view_entry_playable(&v.items[2]));
    assert(view_count_tracks(&v) == 1);

    view_sort(&v, SORT_TITLE, false);
    assert(view_find_path(&v, v.items[0].path) == 0);
    view_remove(&v, 0);
    assert(v.len == 2);
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
    assert(view_count_tracks(&v) == 2);

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
    test_playlist();
    puts("tests: ok");
    return 0;
}
