#ifndef TIT_UI_H
#define TIT_UI_H

#include <stdbool.h>

#include "app.h"

#if !defined(TB_OPT_ATTR_W) || TB_OPT_ATTR_W < 32
#undef TB_OPT_ATTR_W
#define TB_OPT_ATTR_W 64
#endif

/* pkgsrc terminfo lives here on NetBSD; harmless if the path is absent. */
#ifndef TB_TERMINFO_DIR
#define TB_TERMINFO_DIR "/usr/pkg/share/terminfo"
#endif

#include "termbox2.h"
#include "themes.h"

typedef struct {
    bool color;
    int mode;
    const Theme *theme;
    uintattr_t fg;
    uintattr_t bg;
    uintattr_t accent;
    uintattr_t playing;
    uintattr_t dir;
    uintattr_t playlist;
    uintattr_t dim;
    uintattr_t error;
    uintattr_t border;
    bool frame;
} Ui;

int ui_init(Ui *ui);
void ui_shutdown(Ui *ui);
void ui_draw(App *a, Ui *ui);
void ui_handle(App *a, Ui *ui, const struct tb_event *ev);

void ui_set_theme(Ui *ui, const Theme *theme);
bool ui_set_theme_name(Ui *ui, const char *name);
const char *ui_cycle_theme(Ui *ui);

#endif
