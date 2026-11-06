#define TB_IMPL
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static bool bad_cp(uint32_t cp)
{
    if (cp < 0x20 || cp == 0x7f) return true;
    if (cp >= 0x80 && cp <= 0x9f) return true;
    if (cp == 0x200e || cp == 0x200f) return true;
    if (cp >= 0x202a && cp <= 0x202e) return true;
    if (cp >= 0x2066 && cp <= 0x2069) return true;
    return false;
}

static int display_width(const char *s)
{
    int w = 0;
    while (*s) {
        uint32_t cp;
        size_t n = utf8_next(s, &cp);
        s += n;
        if (bad_cp(cp)) cp = ' ';
        int cw = tb_wcwidth(cp);
        if (cw > 0) w += cw;
    }
    return w;
}

/* Returns the column after the printed text. */
static int print_clip(const Ui *ui, int x, int y, int maxw, uintattr_t fg,
                      uintattr_t bg, const char *s, bool ellipsis)
{
    (void)ui;
    int start = x;
    bool cut = false;
    while (*s && x - start < maxw) {
        uint32_t cp;
        size_t n = utf8_next(s, &cp);
        s += n;
        if (cp == 0) break;
        if (bad_cp(cp)) cp = ' ';
        int cw = tb_wcwidth(cp);
        if (cw <= 0) cw = 0;
        if (cw > 1 && x - start + cw > maxw) {
            cut = true;
            break;
        }
        tb_set_cell(x, y, cp, fg, bg);
        x += cw;
        if (!cw) x++;
    }
    if ((cut || *s) && ellipsis && x - start < maxw) {
        tb_set_cell(x, y, 0x2026, fg, bg);
        x++;
    }
    return x;
}

static void print_right(const Ui *ui, int right, int y, int maxw,
                        uintattr_t fg, uintattr_t bg, const char *s)
{
    int w = display_width(s);
    int x = right - w;
    if (x < 0) x = 0;
    if (w > maxw) {
        print_clip(ui, right - maxw, y, maxw, fg, bg, s, true);
    } else {
        print_clip(ui, x, y, maxw, fg, bg, s, false);
    }
}

static const char *entry_title(const Entry *e)
{
    if (e->meta.title && *e->meta.title) return e->meta.title;
    return e->name;
}

static void make_label(const Entry *e, char *buf, size_t size)
{
    if (e->type == ENT_PARENT) {
        snprintf(buf, size, "../");
    } else if (e->type == ENT_DIR) {
        snprintf(buf, size, "%s/", e->name);
    } else if (e->type == ENT_PLAYLIST) {
        snprintf(buf, size, "%s [playlist]", e->name);
    } else if (e->type == ENT_URL) {
        snprintf(buf, size, "[url] %s", e->name);
    } else {
        const char *title = entry_title(e);
        const char *artist = e->meta.artist;
        char body[1200];
        if (artist && *artist)
            snprintf(body, sizeof(body), "%s - %s", artist, title);
        else
            snprintf(body, sizeof(body), "%s", title);
        if (e->meta.track > 0)
            snprintf(buf, size, "%02d. %s", e->meta.track, body);
        else
            snprintf(buf, size, "%s", body);
        if (e->missing) {
            size_t n = strlen(buf);
            if (n + 2 < size) {
                memmove(buf + 2, buf, n + 1);
                buf[0] = '!';
                buf[1] = ' ';
            }
        }
    }
}

static double luminance(uint32_t rgb)
{
    double r = (double)((rgb >> 16) & 0xff) / 255.0;
    double g = (double)((rgb >> 8) & 0xff) / 255.0;
    double b = (double)(rgb & 0xff) / 255.0;
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

static uint32_t blend(uint32_t a, uint32_t b, double t)
{
    int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    int r = (int)(ar * t + br * (1.0 - t) + 0.5);
    int g = (int)(ag * t + bg * (1.0 - t) + 0.5);
    int bl = (int)(ab * t + bb * (1.0 - t) + 0.5);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}

/* Map an RGB colour to the nearest fixed xterm-256 colour. */
static int rgb_to_256(uint32_t rgb)
{
    int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    int ri = (r * 5 + 127) / 255;
    int gi = (g * 5 + 127) / 255;
    int bi = (b * 5 + 127) / 255;
    int cr = ri * 255 / 5, cg = gi * 255 / 5, cb = bi * 255 / 5;
    int cube = 16 + 36 * ri + 6 * gi + bi;
    int dc = (r - cr) * (r - cr) + (g - cg) * (g - cg) + (b - cb) * (b - cb);

    int gray = (r + g + b) / 3;
    int gidx = (gray * 23 + 127) / 255;
    int gv = 8 + gidx * 10;
    int dg = (r - gv) * (r - gv) + (g - gv) * (g - gv) + (b - gv) * (b - gv);
    return dc <= dg ? cube : 232 + gidx;
}

static uintattr_t color_of(const Ui *ui, uint32_t rgb)
{
    if (!ui->color) return TB_DEFAULT;
    if (ui->mode == TB_OUTPUT_TRUECOLOR)
        return rgb ? rgb : TB_HI_BLACK;
    if (ui->mode == TB_OUTPUT_256)
        return (uintattr_t)rgb_to_256(rgb);
    return TB_DEFAULT;
}

void ui_set_theme(Ui *ui, const Theme *theme)
{
    ui->theme = theme ? theme : &themes[0];
    if (!ui->color) {
        ui->fg = ui->bg = ui->accent = ui->playing = ui->dir =
            ui->playlist = ui->dim = ui->error = ui->border = TB_DEFAULT;
        tb_set_clear_attrs(TB_DEFAULT, TB_DEFAULT);
        return;
    }

    if (ui->theme->transparent) {
        if (ui->mode == TB_OUTPUT_NORMAL) {
            ui->fg = ui->bg = TB_DEFAULT;
            ui->accent = TB_YELLOW | TB_BRIGHT;
            ui->dir = TB_BLUE;
            ui->playlist = TB_MAGENTA;
            ui->error = TB_RED;
            ui->dim = TB_BLACK | TB_BRIGHT;
        } else {
            ui->fg = ui->bg = TB_DEFAULT;
            ui->accent = color_of(ui, 0xFFAF00);
            ui->dir = color_of(ui, 0x87AFFF);
            ui->playlist = color_of(ui, 0xD787FF);
            ui->error = color_of(ui, 0xFF5F5F);
            ui->dim = color_of(ui, 0x808080);
        }
        ui->playing = ui->accent;
        ui->border = ui->dim;
        tb_set_clear_attrs(TB_DEFAULT, TB_DEFAULT);
        return;
    }

    if (ui->theme->custom) {
        ui->fg = color_of(ui, ui->theme->fg);
        ui->bg = color_of(ui, ui->theme->bg);
        ui->accent = color_of(ui, ui->theme->accent);
        ui->playing = color_of(ui, ui->theme->playing);
        ui->dir = color_of(ui, ui->theme->dir);
        ui->playlist = color_of(ui, ui->theme->playlist);
        ui->dim = color_of(ui, ui->theme->dim);
        ui->error = color_of(ui, ui->theme->error);
        ui->border = color_of(ui, ui->theme->border);
        tb_set_clear_attrs(ui->fg, ui->bg);
        return;
    }

    bool light = luminance(ui->theme->bg) > 0.55;
    uint32_t fg = ui->theme->fg;
    uint32_t bg = ui->theme->bg;
    uint32_t accent = light ? ui->theme->colors[4] : ui->theme->colors[11];
    uint32_t dir = light ? ui->theme->colors[4] : ui->theme->colors[12];
    uint32_t playlist = light ? ui->theme->colors[5] : ui->theme->colors[13];
    uint32_t error = light ? ui->theme->colors[1] : ui->theme->colors[9];
    uint32_t dim = blend(fg, bg, 0.55);

    if (ui->mode == TB_OUTPUT_NORMAL) {
        ui->fg = light ? TB_BLACK : TB_WHITE;
        ui->bg = light ? TB_WHITE : TB_BLACK;
        ui->accent = light ? TB_BLUE : (TB_YELLOW | TB_BRIGHT);
        ui->dir = TB_BLUE;
        ui->playlist = TB_MAGENTA;
        ui->error = TB_RED;
        ui->dim = light ? TB_BLACK : (TB_BLACK | TB_BRIGHT);
    } else {
        ui->fg = color_of(ui, fg);
        ui->bg = color_of(ui, bg);
        ui->accent = color_of(ui, accent);
        ui->dir = color_of(ui, dir);
        ui->playlist = color_of(ui, playlist);
        ui->error = color_of(ui, error);
        ui->dim = color_of(ui, dim);
    }
    ui->playing = ui->accent;
    ui->border = ui->dim;
    tb_set_clear_attrs(ui->fg, ui->bg);
}

bool ui_set_theme_name(Ui *ui, const char *name)
{
    const Theme *t = theme_by_name(name);
    if (!t) return false;
    ui_set_theme(ui, t);
    return true;
}

const char *ui_cycle_theme(Ui *ui)
{
    size_t i = 0;
    for (; i < theme_count; i++)
        if (&themes[i] == ui->theme) break;
    i = (i + 1) % theme_count;
    ui_set_theme(ui, &themes[i]);
    return themes[i].name;
}

static void draw_header(App *a, Ui *ui, int w)
{
    uintattr_t bg = ui->bg;
    int x = print_clip(ui, 1, 0, w - 2, ui->accent | TB_BOLD, bg,
                       "THE IRON TAPPER", false);
    size_t tracks = view_count_tracks(&a->view);
    char count[64];
    snprintf(count, sizeof(count), "%zu %s", tracks,
             tracks == 1 ? "track" : "tracks");
    int countw = display_width(count);
    int src_max = w - x - 2 - countw - 2;
    if (src_max < 0) src_max = 0;
    print_clip(ui, x + 2, 0, src_max, ui->dim, bg,
               view_title(&a->view), true);
    print_right(ui, w - 2, 0, 12, ui->dim, bg, count);
}

static void draw_list(App *a, Ui *ui, int x0, int y0, int list_w, int list_h)
{
    uintattr_t bg = ui->bg;
    if (!a->view.len) {
        print_clip(ui, x0 + 1, y0 + 1, list_w - 2, ui->dim, bg, "(empty)", false);
        return;
    }
    if (a->cursor < a->top) a->top = a->cursor;
    if (a->cursor >= a->top + (size_t)list_h)
        a->top = a->cursor - (size_t)list_h + 1;
    if (a->top + (size_t)list_h > a->view.len)
        a->top = a->view.len > (size_t)list_h
            ? a->view.len - (size_t)list_h : 0;

    for (int row = 0; row < list_h; row++) {
        size_t i = a->top + (size_t)row;
        if (i >= a->view.len) break;
        Entry *e = &a->view.items[i];
        bool sel = i == a->cursor;
        bool playing = (int)i == a->playing_row;

        uintattr_t fg = ui->fg;
        if (e->type == ENT_PARENT || e->type == ENT_DIR) fg = ui->dir | TB_BOLD;
        else if (e->type == ENT_PLAYLIST) fg = ui->playlist;
        else if (e->type == ENT_URL) fg = ui->dim;
        if (playing) fg = ui->playing | TB_BOLD;
        if (e->type == ENT_FILE && e->missing) fg = ui->error;
        if (sel) fg |= TB_REVERSE;

        int y = y0 + row;
        if (playing)
            tb_set_cell(x0, y, 0x25b6, fg, bg);
        else if (sel)
            tb_set_cell(x0, y, '>', fg, bg);
        char label[2048];
        make_label(e, label, sizeof(label));

        char dur[32] = "";
        int durw = 0;
        if (e->type == ENT_FILE && e->meta.duration_ms > 0) {
            format_time(dur, sizeof(dur), (uint64_t)e->meta.duration_ms);
            durw = 7;
        }
        int right = x0 + list_w - 1;
        int label_max = right - (x0 + 1) - (durw ? durw + 1 : 0);
        if (label_max < 1) label_max = 1;
        print_clip(ui, x0 + 1, y, label_max, fg, bg, label, true);
        if (durw)
            print_right(ui, right, y, durw, fg, bg, dur);
    }
}

static void draw_now(App *a, Ui *ui, int w, int y)
{
    uintattr_t bg = ui->bg;
    const QTrack *now = app_now(a);
    uint32_t state = '.';
    uintattr_t state_fg = ui->dim;
    if (player_is_playing(a->player)) {
        state = 0x25b6;
        state_fg = ui->playing | TB_BOLD;
    } else if (player_is_paused(a->player)) {
        state = '=';
        state_fg = ui->accent;
    } else if (player_has_track(a->player)) {
        state = '|';
    }

    tb_set_cell(1, y, state, state_fg, bg);
    if (!now || !player_has_track(a->player)) {
        print_clip(ui, 3, y, w - 4, ui->dim, bg, "(nothing playing)", false);
        return;
    }
    const char *title = now->meta.title && *now->meta.title
        ? now->meta.title : path_basename(now->path);
    char line[2048];
    if (now->meta.artist && *now->meta.artist)
        snprintf(line, sizeof(line), "%s - %s", now->meta.artist, title);
    else
        snprintf(line, sizeof(line), "%s", title);

    char fmt[64];
    int hz = player_hz(a->player);
    int br = player_bitrate(a->player);
    snprintf(fmt, sizeof(fmt), "%d Hz/%dch/%dk", hz,
             player_channels(a->player), br);
    int fmtw = display_width(fmt);
    int title_max = w - 4 - fmtw - 1;
    if (title_max < 1) title_max = 1;
    print_clip(ui, 3, y, title_max, ui->fg | TB_BOLD, bg, line, true);
    print_right(ui, w - 2, y, 18, ui->dim, bg, fmt);
}

static void draw_progress(App *a, Ui *ui, int w, int y)
{
    uintattr_t bg = ui->bg;
    uint64_t pos = player_has_track(a->player) ? player_pos_frames(a->player) : 0;
    uint64_t len = player_has_track(a->player) ? player_len_frames(a->player) : 0;
    int hz = player_hz(a->player);
    const QTrack *now = app_now(a);
    if (player_has_track(a->player) && !len && now && now->meta.duration_ms && hz)
        len = (uint64_t)now->meta.duration_ms * (uint64_t)hz / 1000;
    uint64_t pos_ms = hz ? pos * 1000 / (uint64_t)hz : 0;
    uint64_t len_ms = hz ? len * 1000 / (uint64_t)hz : 0;
    char left[32], right[32];
    format_time(left, sizeof(left), pos_ms);
    format_time(right, sizeof(right), len_ms ? len_ms : 0);
    if (!len) snprintf(right, sizeof(right), "--:--");

    print_clip(ui, 1, y, 8, ui->dim, bg, left, false);
    print_right(ui, w - 2, y, 8, ui->dim, bg, right);

    int bar_x = 9;
    int bar_w = w - 2 - 8 - bar_x;
    if (bar_w < 4) return;
    int fill = 0;
    if (len) {
        fill = (int)(pos * (uint64_t)bar_w / len);
        if (fill > bar_w) fill = bar_w;
    }
    uintattr_t fg = ui->playing;
    uintattr_t rest = ui->dim;
    if (player_is_paused(a->player)) fg = ui->dim;
    for (int i = 0; i < bar_w; i++) {
        if (i < fill) tb_set_cell(bar_x + i, y, 0x2501, fg, bg);
        else tb_set_cell(bar_x + i, y, 0x2500, rest, bg);
    }
}

static void draw_footer(App *a, Ui *ui, int w, int y)
{
    uintattr_t bg = ui->bg;
    const char *st = app_status(a);
    if (st) {
        print_clip(ui, 1, y, w - 10, ui->accent, bg, st, true);
    } else {
        char line[256];
        snprintf(line, sizeof(line),
                 "vol %d%%  shuf %s  rep %s  sort %s%s",
                 (int)(player_volume(a->player) * 100.0f + 0.5f),
                 a->shuffle ? "on" : "off",
                 app_repeat_name(a->repeat),
                 app_sort_name(a->sort),
                 a->reverse ? " rev" : "");
        print_clip(ui, 1, y, w - 10, ui->dim, bg, line, true);
    }
    print_right(ui, w - 2, y, 8, ui->dim, bg, "? help");
}

static void draw_help(Ui *ui, int w, int h)
{
    uintattr_t bg = ui->bg;
    print_clip(ui, 1, 0, w - 2, ui->accent | TB_BOLD, bg,
               "THE IRON TAPPER — keys", false);
    static const char *lines[] = {
        "j/k, Up/Down    move            Enter   play / enter dir / load playlist",
        "g/G, Home/End   top / bottom    Space   play / pause",
        "PgUp/PgDn       page            n / p   next / previous track",
        "Left/Right      seek -/+ 5s     x       stop",
        "+ / -           volume          m       mute",
        "s               shuffle         r       repeat off/all/one",
        "o               sort mode       O       reverse order",
        "t               next color theme",
        "f               frame around the list",
        "d               remove from list (memory only)",
        "b / Backspace   parent directory",
        "R               reload directory / playlist",
        "q               quit",
        "",
        "Filesystem is the playlist: directories show MP3s and subdirectories.",
        "Deleting a track only removes it from the in-memory list.",
        "",
        "press any key",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        if ((int)i + 2 >= h) break;
        print_clip(ui, 2, (int)i + 2, w - 4,
                   i == sizeof(lines) / sizeof(lines[0]) - 1
                       ? ui->dim : ui->fg,
                   bg, lines[i], true);
    }
}

static void draw_frame(Ui *ui, int w, int h)
{
    int x0 = 1, y0 = 1, x1 = w - 2, y1 = h - 4;
    uintattr_t fg = ui->border;
    uintattr_t bg = ui->bg;
    tb_set_cell(x0, y0, 0x256d, fg, bg);
    tb_set_cell(x1, y0, 0x256e, fg, bg);
    tb_set_cell(x0, y1, 0x2570, fg, bg);
    tb_set_cell(x1, y1, 0x256f, fg, bg);
    for (int x = x0 + 1; x < x1; x++) {
        tb_set_cell(x, y0, 0x2500, fg, bg);
        tb_set_cell(x, y1, 0x2500, fg, bg);
    }
    for (int y = y0 + 1; y < y1; y++) {
        tb_set_cell(x0, y, 0x2502, fg, bg);
        tb_set_cell(x1, y, 0x2502, fg, bg);
    }
}

void ui_draw(App *a, Ui *ui)
{
    int w = tb_width();
    int h = tb_height();
    tb_clear();
    if (w < 30 || h < 8) {
        const char *msg = "terminal too small (min 30x8)";
        print_clip(ui, 1, 0, w - 1, ui->error, ui->bg, msg, true);
        tb_present();
        return;
    }
    if (a->help) {
        draw_help(ui, w, h);
        tb_present();
        return;
    }
    draw_header(a, ui, w);
    int list_x = 1, list_y = 1, list_w = w - 2, list_h = h - 4;
    if (ui->frame && w >= 34 && h >= 10) {
        draw_frame(ui, w, h);
        list_x = 2;
        list_y = 2;
        list_w = w - 4;
        list_h = h - 6;
    }
    draw_list(a, ui, list_x, list_y, list_w, list_h);
    draw_now(a, ui, w, h - 3);
    draw_progress(a, ui, w, h - 2);
    draw_footer(a, ui, w, h - 1);
    tb_present();
}

void ui_handle(App *a, Ui *ui, const struct tb_event *ev)
{
    if (ev->type == TB_EVENT_RESIZE) return;
    if (a->help) {
        a->help = false;
        return;
    }
    if (ev->type != TB_EVENT_KEY) return;

    switch (ev->key) {
    case TB_KEY_CTRL_C:
        a->quit = true;
        return;
    case TB_KEY_ENTER:
        app_activate(a);
        return;
    case TB_KEY_ESC:
        return;
    case TB_KEY_BACKSPACE:
    case TB_KEY_BACKSPACE2:
        app_back(a);
        return;
    case TB_KEY_ARROW_UP:
        app_move(a, -1);
        return;
    case TB_KEY_ARROW_DOWN:
        app_move(a, 1);
        return;
    case TB_KEY_ARROW_LEFT:
        app_seek(a, -5000);
        return;
    case TB_KEY_ARROW_RIGHT:
        app_seek(a, 5000);
        return;
    case TB_KEY_PGUP:
        app_page(a, -1, tb_height() - 4);
        return;
    case TB_KEY_PGDN:
        app_page(a, 1, tb_height() - 4);
        return;
    case TB_KEY_HOME:
        app_home(a);
        return;
    case TB_KEY_END:
        app_end(a);
        return;
    default:
        break;
    }

    switch (ev->ch) {
    case 3: /* Ctrl-C: termbox2 disables ISIG */
    case 'q':
    case 'Q':
        a->quit = true;
        break;
    case '?':
        a->help = true;
        break;
    case 'j':
        app_move(a, 1);
        break;
    case 'k':
        app_move(a, -1);
        break;
    case 'g':
        app_home(a);
        break;
    case 'G':
        app_end(a);
        break;
    case ' ':
        app_play_pause(a);
        break;
    case 'n':
        app_next(a);
        break;
    case 'p':
        app_prev(a);
        break;
    case 'x':
        app_stop(a);
        break;
    case '+':
    case '=':
        app_volume_delta(a, 5);
        break;
    case '-':
        app_volume_delta(a, -5);
        break;
    case 'm':
        app_mute(a);
        break;
    case 's':
        app_toggle_shuffle(a);
        break;
    case 'r':
        app_cycle_repeat(a);
        break;
    case 'o':
        app_cycle_sort(a);
        break;
    case 'O':
        app_toggle_reverse(a);
        break;
    case 't':
    case 'T': {
        char buf[128];
        snprintf(buf, sizeof(buf), "theme: %s", ui_cycle_theme(ui));
        app_note(a, buf);
        break;
    }
    case 'f':
    case 'F':
        ui->frame = !ui->frame;
        app_note(a, ui->frame ? "frame on" : "frame off");
        break;
    case 'd':
        app_delete(a);
        break;
    case 'b':
        app_back(a);
        break;
    case 'R':
        app_reload(a);
        break;
    default:
        break;
    }
}

int ui_init(Ui *ui)
{
    if (tb_init() != 0) return -1;
    ui->color = getenv("NO_COLOR") == NULL;

    bool truecolor = false;
    const char *colorterm = getenv("COLORTERM");
    if (colorterm && (strstr(colorterm, "truecolor") ||
                      strstr(colorterm, "24bit")))
        truecolor = true;
    const char *term = getenv("TERM");
    if (term && strstr(term, "direct")) truecolor = true;
    const char *override = getenv("TIT_TRUECOLOR");
    if (override) truecolor = atoi(override) != 0;

    if (!ui->color) {
        ui->mode = TB_OUTPUT_NORMAL;
        tb_set_output_mode(TB_OUTPUT_NORMAL);
    } else if (truecolor) {
        ui->mode = TB_OUTPUT_TRUECOLOR;
        tb_set_output_mode(TB_OUTPUT_TRUECOLOR);
    } else {
        ui->mode = TB_OUTPUT_256;
        tb_set_output_mode(TB_OUTPUT_256);
    }
    tb_hide_cursor();
    ui->frame = false;
    ui_set_theme(ui, &themes[0]);
    return 0;
}

void ui_shutdown(Ui *ui)
{
    (void)ui;
    tb_shutdown();
}
