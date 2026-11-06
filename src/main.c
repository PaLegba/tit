#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "player.h"
#include "tags.h"
#include "ui.h"
#include "util.h"
#include "termbox2.h"

#define TIT_VERSION "0.1"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *f)
{
    fputs(
        "usage: tit [options] [path ...]\n"
        "\n"
        "  path may be an MP3 file, a directory, or an .m3u/.m3u8/.pls playlist.\n"
        "  With no path, the current directory is opened.\n"
        "\n"
        "  -n, --no-audio              use the null audio backend\n"
        "      --tag-encoding=ENC      latin1 (default), cp1251 or utf8\n"
        "      --theme=NAME            color theme (see --theme=list)\n"
        "      --frame                 draw a frame around the list\n"
        "  -h, --help                  show this help\n"
        "  -v, --version               show version\n",
        f);
}

int main(int argc, char **argv)
{
    TagEncoding enc = TAG_LATIN1;
    bool null_audio = false;
    bool frame = false;
    const char *theme_name = NULL;
    const char *paths[256];
    size_t npaths = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            printf("tit %s\n", TIT_VERSION);
            return 0;
        } else if (strcmp(arg, "-n") == 0 || strcmp(arg, "--no-audio") == 0) {
            null_audio = true;
        } else if (strcmp(arg, "--frame") == 0) {
            frame = true;
        } else if (strcmp(arg, "--tag-encoding=cp1251") == 0) {
            enc = TAG_CP1251;
        } else if (strcmp(arg, "--tag-encoding=latin1") == 0) {
            enc = TAG_LATIN1;
        } else if (strcmp(arg, "--tag-encoding=utf8") == 0) {
            enc = TAG_UTF8;
        } else if (strcmp(arg, "--theme=list") == 0) {
            for (size_t i = 0; i < theme_count; i++)
                printf("%s\n", themes[i].name);
            return 0;
        } else if (strncmp(arg, "--theme=", 8) == 0) {
            theme_name = arg + 8;
        } else if (strcmp(arg, "--") == 0) {
            for (i++; i < argc; i++) {
                if (npaths < sizeof(paths) / sizeof(paths[0]))
                    paths[npaths++] = argv[i];
            }
        } else if (arg[0] == '-' && arg[1]) {
            fprintf(stderr, "tit: unknown option: %s\n", arg);
            usage(stderr);
            return 2;
        } else {
            if (npaths < sizeof(paths) / sizeof(paths[0]))
                paths[npaths++] = arg;
            else
                fprintf(stderr, "tit: too many paths, ignoring %s\n", arg);
        }
    }

    setlocale(LC_CTYPE, "");

    char err[256];
    Player *player = player_create(null_audio, err, sizeof(err));
    if (!player) {
        fprintf(stderr, "tit: %s\n", err);
        return 1;
    }

    App app;
    app_init(&app, player, enc);

    Ui ui;
    if (ui_init(&ui) != 0) {
        fprintf(stderr, "tit: cannot initialize terminal\n");
        player_destroy(player);
        return 1;
    }
    ui.frame = frame;

    if (npaths == 0) {
        app_open_dir(&app, ".");
    } else if (npaths == 1) {
        app_open_path(&app, paths[0], true);
    } else {
        bool all_files = true;
        for (size_t i = 0; i < npaths; i++) {
            struct stat st;
            if (stat(paths[i], &st) != 0 || !S_ISREG(st.st_mode) ||
                !is_mp3_name(paths[i])) {
                all_files = false;
                break;
            }
        }
        if (all_files) {
            app_open_files(&app, (char *const *)paths, npaths);
        } else {
            app_open_path(&app, paths[0], true);
            for (size_t i = 1; i < npaths; i++)
                fprintf(stderr, "tit: ignoring extra path: %s\n", paths[i]);
        }
    }

    if (theme_name && !ui_set_theme_name(&ui, theme_name)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown theme: %s", theme_name);
        app_note(&app, msg);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    while (!app.quit && !g_stop) {
        app_tick(&app);
        ui_draw(&app, &ui);
        struct tb_event ev;
        int r = tb_peek_event(&ev, 100);
        if (r == TB_OK) {
            ui_handle(&app, &ui, &ev);
        } else if (r == TB_ERR_NO_EVENT) {
            continue;
        } else if (r < 0) {
            if (r == TB_ERR_POLL && tb_last_errno() == EINTR) continue;
            break;
        }
    }

    ui_shutdown(&ui);
    app_free(&app);
    player_destroy(player);
    return 0;
}
