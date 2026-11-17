/* fileio.c - file statements (DOS file functions only) */
#include "basic.h"
#include "vessel.h"
#include <ctype.h>
#ifdef __WATCOMC__
#include <dos.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define MAXF 16
static struct { FILE *fp; char mode; } ftab[MAXF];

void file_fname(const char *in, unsigned len, char *out, const char *defext)
{
    unsigned i, o = 0;
    int hasdot = 0;
    while (len > 0 && *in == ' ') { in++; len--; }
    while (len > 0 && in[len - 1] == ' ') len--;
    if (len == 0 || len > 70) berror(E_BAD_FNAME);
    for (i = 0; i < len; i++) {
        char c = in[i];
        if (c == '.') hasdot = 1;
        if (c == '\\' || c == ':' || c == '/') hasdot = 0;
        out[o++] = c;
    }
    out[o] = 0;
    if (!hasdot && defext) strcat(out, defext);
#ifdef __WATCOMC__
    for (i = 0; out[i]; i++) out[i] = (char)toupper((u8)out[i]);
#endif
}

int parse_filenum(void)
{
    int n;
    accept('#');
    n = eval_int();
    if (n < 1 || n >= MAXF) berror(E_BAD_FILENO);
    return n;
}

FILE *file_get(int n, int mode)
{
    if (n < 1 || n >= MAXF || !ftab[n].fp) berror(E_BAD_FILENO);
    if (mode == 'I' && ftab[n].mode != 'I') berror(E_BAD_MODE);
    if (mode == 'O' && ftab[n].mode == 'I') berror(E_BAD_MODE);
    return ftab[n].fp;
}

void file_close_all(void)
{
    int i;
    for (i = 1; i < MAXF; i++) if (ftab[i].fp) {
        if (ftab[i].mode != 'I') fputc(0x1A, ftab[i].fp);
        fclose(ftab[i].fp); ftab[i].fp = 0;
    }
}

static void do_open(const char *fn, int mode, int n)
{
    FILE *fp;
    int i;
    if (n < 1 || n >= MAXF) berror(E_BAD_FILENO);
    if (ftab[n].fp) berror(E_FILE_OPEN);
    for (i = 1; i < MAXF; i++)
        if (ftab[i].fp && mode != 'I' && 0) berror(E_FILE_OPEN);
    if (mode == 'I') fp = fopen(fn, "rb");
    else if (mode == 'O') fp = fopen(fn, "wb");
    else if (mode == 'A') {
        /* find size and whether the file ends with ^Z, without mixing read/write */
        long sz = -1; int last = -1;
        fp = fopen(fn, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END); sz = ftell(fp);
            if (sz > 0) { fseek(fp, sz - 1, SEEK_SET); last = fgetc(fp); }
            fclose(fp);
        }
        if (sz < 0) fp = fopen(fn, "wb");
        else {
            fp = fopen(fn, "r+b");
            if (fp) fseek(fp, (last == 0x1A) ? sz - 1 : sz, SEEK_SET);
        }
    } else { berror(E_BAD_MODE); return; }
    if (!fp) berror(mode == 'I' ? E_FILE_NF : E_IO);
    ftab[n].fp = fp; ftab[n].mode = (char)mode;
}

void stmt_open(void)
{
    VAL a; char fn[80];
    int mode = 0, n;
    eval_str(&a);
    if (accept(',')) {
        /* OPEN "I",#1,"file" */
        char m = a.u.s.len ? (char)toupper((u8)a.u.s.p[0]) : 0;
        VAL f;
        n = parse_filenum();
        expect(',');
        eval_str(&f);
        if (accept(',')) eval_int();
        file_fname(f.u.s.p, f.u.s.len, fn, 0);
        if (m != 'I' && m != 'O' && m != 'A') berror(E_BAD_MODE);
        do_open(fn, m, n);
        return;
    }
    file_fname(a.u.s.p, a.u.s.len, fn, 0);
    if (accept(T_FOR)) {
        if (accept(T_INPUT)) mode = 'I';
        else if (accept(T_OUTPUT)) mode = 'O';
        else if (accept(T_APPEND)) mode = 'A';
        else syntax();
    } else berror(E_BAD_MODE);
    expect(T_AS);
    n = parse_filenum();
    if (accept_func(F_LEN)) { expect('='); eval_int(); }
    do_open(fn, mode, n);
}

void stmt_close(void)
{
    if (at_stmt_end()) { file_close_all(); return; }
    do {
        int n = parse_filenum();
        if (ftab[n].fp) {
            if (ftab[n].mode != 'I') fputc(0x1A, ftab[n].fp);
            fclose(ftab[n].fp); ftab[n].fp = 0;
        }
    } while (accept(','));
}

int fn_eof(int n)
{
    FILE *fp = file_get(n, 0);
    int c;
    if (ftab[n].mode != 'I') return -1;
    c = fgetc(fp);
    if (c == EOF || c == 0x1A) { if (c != EOF) ungetc(c, fp); return 1; }
    ungetc(c, fp);
    return 0;
}

long fn_lof(int n)
{
    FILE *fp = file_get(n, 0);
    long cur = ftell(fp), sz;
    fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, cur, SEEK_SET);
    return sz;
}

long fn_loc(int n)
{
    FILE *fp = file_get(n, 0);
    return (ftell(fp) + 127) / 128;
}

void stmt_kill(void)
{
    VAL a; char fn[80];
    eval_str(&a);
    file_fname(a.u.s.p, a.u.s.len, fn, 0);
    if (remove(fn) != 0) berror(E_FILE_NF);
}

void stmt_name(void)
{
    VAL a, b; char f1[80], f2[80];
    eval_str(&a); expect(T_AS); eval_str(&b);
    file_fname(a.u.s.p, a.u.s.len, f1, 0);
    file_fname(b.u.s.p, b.u.s.len, f2, 0);
    {
        FILE *t = fopen(f2, "rb");
        if (t) { fclose(t); berror(E_FILE_EXISTS); }
    }
    if (rename(f1, f2) != 0) berror(E_FILE_NF);
}

void stmt_files(void)
{
    char spec[80];
    int col = 0;
    if (!at_stmt_end()) {
        VAL a; eval_str(&a);
        file_fname(a.u.s.p, a.u.s.len, spec, 0);
        if (spec[strlen(spec) - 1] == ':' || spec[strlen(spec) - 1] == '\\') strcat(spec, "*.*");
    } else strcpy(spec, "*.*");
#ifdef __WATCOMC__
    {
        struct find_t ft;
        unsigned r = _dos_findfirst(spec, _A_NORMAL | _A_SUBDIR | _A_RDONLY | _A_ARCH, &ft);
        if (r) berror(E_FILE_NF);
        while (!r) {
            char b[20], nm[9], ex[4];
            char *dot = strchr(ft.name, '.');
            int i;
            if (ft.name[0] != '.') {
                memset(nm, 0, sizeof(nm)); memset(ex, 0, sizeof(ex));
                if (dot) { memcpy(nm, ft.name, dot - ft.name); strncpy(ex, dot + 1, 3); }
                else strncpy(nm, ft.name, 8);
                sprintf(b, "%-8s.%-3s%c ", nm, ex, (ft.attrib & _A_SUBDIR) ? '<' : ' ');
                for (i = 0; b[i]; i++) if (b[i] == 0) b[i] = ' ';
                if (col + 14 > scr_w) { con_newline(); col = 0; }
                con_puts(b); col += (int)strlen(b);
            }
            if (hal_breakflag()) break;
            r = _dos_findnext(&ft);
        }
    }
#else
    {
        DIR *d = opendir(".");
        struct dirent *e;
        if (!d) berror(E_FILE_NF);
        while ((e = readdir(d)) != 0) {
            char b[40];
            if (e->d_name[0] == '.') continue;
            sprintf(b, "%-13.13s ", e->d_name);
            if (col + 14 > scr_w) { con_newline(); col = 0; }
            con_puts(b); col += (int)strlen(b);
        }
        closedir(d);
    }
#endif
    if (col) con_newline();
}

void stmt_bsave(void)
{
    VAL a; char fn[80]; long off, len, i;
    FILE *fp;
    eval_str(&a); expect(','); off = eval_long(); expect(','); len = eval_long();
    file_fname(a.u.s.p, a.u.s.len, fn, 0);
    fp = fopen(fn, "wb");
    if (!fp) berror(E_IO);
    fputc(0xFD, fp);
    fputc(def_seg & 0xFF, fp); fputc((def_seg >> 8) & 0xFF, fp);
    fputc((int)(off & 0xFF), fp); fputc((int)((off >> 8) & 0xFF), fp);
    fputc((int)(len & 0xFF), fp); fputc((int)((len >> 8) & 0xFF), fp);
    for (i = 0; i < len; i++) fputc(hal_peek(def_seg, (unsigned)(off + i)), fp);
    fclose(fp);
}

void stmt_bload(void)
{
    VAL a; char fn[80]; long off = -1, len, i;
    unsigned seg = def_seg;
    FILE *fp;
    int c;
    int run_after = 0;
    eval_str(&a);
    if (accept(',')) { if (peekc() != ',' && !at_stmt_end() && toupper(peekc()) != 'R') off = eval_long(); if (accept(',')) { if (toupper(peekc()) == 'R') { txtptr++; run_after = 1; } } else if (toupper(peekc()) == 'R') { txtptr++; run_after = 1; } /* PC-88 style: BLOAD "file",R */ }
    else if (!at_stmt_end() && toupper(peekc()) == 'R') { txtptr++; run_after = 1; }
    file_fname(a.u.s.p, a.u.s.len, fn, 0);
    fp = fopen(fn, "rb");
    if (!fp) berror(E_FILE_NF);
    c = fgetc(fp);
    if (c == 0xFD) {
        unsigned fseg = fgetc(fp); fseg |= fgetc(fp) << 8;
        {
            unsigned foff = fgetc(fp); foff |= fgetc(fp) << 8;
            len = fgetc(fp); len |= (long)fgetc(fp) << 8;
            if (off < 0) { off = foff; seg = fseg; }
        }
        for (i = 0; i < len; i++) { c = fgetc(fp); if (c == EOF) break; hal_poke(seg, (unsigned)(off + i), c); }
    } else {
        /* raw image: default to the machine area above CLEAR ,m */
        if (off < 0) off = p8_mem_top ? (long)((p8_mem_top + 1) & 0xFFFF) : 0x8000L;
        i = 0;
        while (c != EOF) { hal_poke(seg, (unsigned)(off + i), c); i++; c = fgetc(fp); }
    }
    fclose(fp);
    vessel_notify_load((unsigned)off, i);
    if (run_after) { vessel_init(); vessel_exec((unsigned)off); }
}

long dos_diskfree(int drive)
{
#ifdef __WATCOMC__
    struct diskfree_t df;
    if (_dos_getdiskfree(drive, &df)) berror(E_IO);
    return (long)df.avail_clusters * df.sectors_per_cluster * df.bytes_per_sector / 1024L;
#else
    (void)drive;
    return 1024L;
#endif
}
