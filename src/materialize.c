/* materialize.c - see materialize.h. Moved out of engine.c so the
 * host CLI can boot from a D88 exactly the way the embedded
 * engine does, instead of only finding plain files in the
 * working directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "materialize.h"
#include "p88disk.h"
#include "p88conv.h"

char *p8_materialize(char *bootfile, size_t bootfile_cap)
{
    P88FileInfo files[64];
    static unsigned char buf[1 << 20];
    char tmpl[256];
    char boot[32];
    const char *tmp = getenv("TMPDIR");
    char *workdir;
    int n, i;
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/p8pXXXXXX", tmp);
    if (!mkdtemp(tmpl)) return 0;
    workdir = strdup(tmpl);
    if (!workdir) return 0;
    n = p88disk_list(files, 64);
    if (n < 0) { free(workdir); return 0; }
    boot[0] = 0;
    for (i = 0; i < n; i++) {
        long sz = p88disk_read(files[i].name, buf, sizeof(buf));
        char path[320], base[16];
        const char *dot;
        FILE *fp;
        if (sz < 0) continue;
        /* base name without extension, for the .BAS materialization */
        memset(base, 0, sizeof(base));
        strncpy(base, files[i].name, sizeof(base) - 1);
        dot = strrchr(base, '.');
        if (files[i].attr & 0x80) {
            char *text, *report = 0;
            unsigned long tlen = 0;
            char matname[32];
            if (dot) {
                /* A tokenized file whose disk name carries an
                 * extension (MENU.ALP) materializes as
                 * MENU_ALP.BAS: writing MENU.BAS would collide
                 * case-insensitively with a distinct program
                 * named "menu" (iOS returns EEXIST), and the
                 * guest only ever asks for the extension-less
                 * name. */
                char ext[4];
                snprintf(ext, sizeof(ext), "%.3s", dot + 1);
                *(char *)dot = 0;
                snprintf(matname, sizeof(matname), "%.10s_%.3s.BAS",
                         base, ext);
            } else {
                snprintf(matname, sizeof(matname), "%.15s.BAS", base);
            }
            text = p88_convert(buf, (unsigned long)sz, &tlen, &report);
            free(report);
            if (!text) continue;
            snprintf(path, sizeof(path), "%s/%s", workdir, matname);
            fp = fopen(path, "wb");
            if (fp) { fwrite(text, 1, tlen, fp); fclose(fp); }
            free(text);
            if (!boot[0] || strcmp(base, "MENU") == 0)
                snprintf(boot, sizeof(boot), "%s", matname);
        } else {
            snprintf(path, sizeof(path), "%s/%.15s", workdir, files[i].name);
            fp = fopen(path, "wb");
            if (fp) { fwrite(buf, 1, (size_t)sz, fp); fclose(fp); }
        }
    }
    if (bootfile && bootfile_cap) {
        snprintf(bootfile, bootfile_cap, "%s", boot);
        if (!boot[0]) { free(workdir); return 0; }
    }
    return workdir;
}
