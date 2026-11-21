/* textmerge_test.c - unit checks for the engine's text-screen
 * merge (p8_text_codes / p8_text_attrs vs the guest text image at
 * 0xF3C8). No guest runs: the image is poked directly and the
 * interpreter surface is written through hal_tput, then the merged
 * result is asserted cell by cell. Exit 0 = all checks pass. */
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "hal.h"
#include "halp8.h"

#define IMG 0xF3C8
static int fails;
static void chk(const char *what, long got, long want)
{
    if (got != want) { printf("FAIL %s: got %ld want %ld\n", what, got, want); fails++; }
}
static void poke(int addr, int v) { hal_poke(0, addr, v); }
static void img_code(int x, int y, int v) { poke(IMG + y * 120 + x, v); }
static void img_pair(int y, int j, int col, int attr)
{
    poke(IMG + y * 120 + 80 + j * 2, col);
    poke(IMG + y * 120 + 80 + j * 2 + 1, attr);
}

int main(void)
{
    P8Engine *e = p8_create("/nonexistent.d88");
    const unsigned short *codes;
    const unsigned char *attrs;
    int i;
    if (!e) { printf("FAIL create\n"); return 1; }
    for (i = 0; i < 25 * 120; i++) poke(IMG + i, 0);

    /* row 0: image ASCII with a yellow row default (0xC8) */
    img_code(0, 0, 'A'); img_code(1, 0, 'B');
    img_pair(0, 0, 0x80, 0xC8);
    /* row 1: image kanji (SJIS 82 A0 = JIS 24 22) */
    img_code(0, 1, 0x82); img_code(1, 1, 0xA0);
    /* row 2: semi-graphic row (pair 0x98 = green + graph) */
    img_pair(2, 0, 0x80, 0x98);
    img_code(0, 2, 0x00); img_code(1, 2, 0x20); img_code(2, 2, 0xFF);
    /* row 3: secret decoration hides the image codes */
    img_pair(3, 0, 0x80, 0x01);
    img_code(0, 3, 'X');
    /* row 4: interpreter text recolored by a marker pair at col 4.
     * Pair 0 (decoration off) clears the secret state row 3 left
     * behind - color commands do not touch decorations - but it
     * keeps row 2's graphic flag, so pair 1 (white color command
     * at column 2) turns graphic off before 'M'; pair 2 at
     * column 4 is the cyan marker 0xA8. */
    hal_tput(2, 4, 'M', 0xE1); hal_tput(6, 4, 'N', 0xE1);
    img_pair(4, 0, 0x80, 0x00);
    img_pair(4, 1, 2, 0xE8);
    img_pair(4, 2, 4, 0xA8);
    /* row 5: untouched - interpreter cell passes through */
    hal_tput(0, 5, 'Z', 0xE1);
    /* row 6: image space does not shadow interpreter text, and a
     * black color pair (0x08: no color bits, no graph) does not
     * override the interpreter's attr */
    hal_tput(0, 6, 'Q', 0xE1);
    img_code(0, 6, 0x20); img_pair(6, 0, 0x80, 0x08);

    codes = p8_text_codes(e);
    attrs = p8_text_attrs(e);

    chk("r0c0 code", codes[0], 'A');
    chk("r0c1 code", codes[1], 'B');
    chk("r0c0 attr", attrs[0], 0xC0);
    chk("r1 kanji left", codes[80], 0x2204);
    chk("r1 kanji right", codes[81], 0x2284);
    chk("r2 graph 00", codes[160], 0x00);
    chk("r2 graph 20", codes[161], 0x20);
    chk("r2 graph FF", codes[162], 0xFF);
    chk("r2 graph attr", attrs[162], 0x90);
    chk("r3 secret hidden", codes[240], 0);
    chk("r4 before marker code", codes[320 + 2], 'M');
    chk("r4 before marker attr", attrs[320 + 2], 0xE1);
    chk("r4 marker code", codes[320 + 6], 'N');
    chk("r4 marker attr", attrs[320 + 6], 0xA0);
    chk("r5 passthrough code", codes[400], 'Z');
    chk("r5 passthrough attr", attrs[400], 0xE1);
    chk("r6 space no shadow", codes[480], 'Q');
    chk("r6 black pair no override", attrs[480], 0xE1);

    if (fails) { printf("textmerge: %d FAIL\n", fails); return 1; }
    printf("textmerge ok\n");
    return 0;
}
