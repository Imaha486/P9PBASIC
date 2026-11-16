/* main.c - P9PBASIC entry point */
#include "basic.h"
#include <ctype.h>

void interp_init(void);
void exec_direct_pending_set(const char *s);

int main(int argc, char **argv)
{
    const char *file = 0;
    int i, keep = 0, rawinp = 0;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '/' || argv[i][0] == '-') {
            char c = (char)toupper((u8)argv[i][1]);
            if (c == 'R') rawinp = 1;
            else if (c == 'K') keep = 1;
            else if (c == 'G') { extern int ank_mode; ank_mode = 1; }
        } else if (!file) file = argv[i];
    }
    interp_init();
    hal_init(0);
    hal_set_inp_direct(rawinp);
    scr_init();
    g_init();
    if (file) {
        char fn[80];
        file_fname(file, (unsigned)strlen(file), fn, ".BAS");
        exit_after_run = !keep;
        if (!prog_load(fn, 0)) {
            con_puts("\x83t\x83@\x83" "C\x83\x8B\x82\xAA\x8C\xA9\x82\xC2\x82\xA9\x82\xE8\x82\xDC\x82\xB9\x82\xF1: ");  /* ファイルが見つかりません: */ con_puts(fn); con_newline();
            exit_after_run = 0;
            top_level();
        }
        ok_needed = 0;
        exec_direct_pending_set("RUN");
    } else {
        char b[64];
        con_puts(VERSION_STR); con_newline();
        sprintf(b, "\x83v\x83\x8D\x83O\x83\x89\x83\x80\x97\xCC\x88\xE6 %u \x83o\x83" "C\x83g", prog_free());  /* プログラム領域 %u バイト */
        con_puts(b); con_newline();
    }
    top_level();
    hal_term();
    return 0;
}
