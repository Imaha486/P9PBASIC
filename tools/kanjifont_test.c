/* Kanji font table checks.
 *
 * The expected indices below were verified against MA2026's actual
 * kanji-ROM port traffic (the guest latches these addresses when it
 * plots the panel text) and the NEC ROM address map described by the
 * QUASI88 emulator (kanji2addr):
 *   "PC-88... MUSIC ARENA 2026 ... PART2" header and song comments.
 * Glyph shapes come from the Shinonome font (public domain); this
 * test pins placement, not outlines.
 */
#include <stdio.h>
#include <string.h>

extern const unsigned char p8_kanji_glyph[2][4096][32];

static int failures;

static int ink(int bank, int idx)
{
    int n = 0, i;
    for (i = 0; i < 32; i++) {
        unsigned char b = p8_kanji_glyph[bank][idx][i];
        while (b) { n += b & 1; b >>= 1; }
    }
    return n;
}

static void expect(int bank, int idx, int min_ink, const char *what)
{
    int n = ink(bank, idx);
    if (n < min_ink) {
        printf("FAIL %s: bank %d idx 0x%03X ink %d < %d\n",
               what, bank, idx, n, min_ink);
        failures++;
    } else {
        printf("ok   %s: bank %d idx 0x%03X ink %d\n", what, bank, idx, n);
    }
}

int main(void)
{
    /* Trace-verified placements (MA2026 panel text). */
    expect(0, 0x26D, 30, "FULLWIDTH M (JIS 234D)");
    expect(0, 0x65B, 40, "ON/oto kanji (JIS 323B)");
    expect(0, 0x13C, 5,  "long-vowel mark (JIS 213C)");
    expect(0, 0x1B7, 10, "katakana SHI (JIS 2537)");
    expect(0, 0x7B8, 50, "SHUU/atsumu kanji (JIS 3D38)");
    expect(0, 0x270, 20, "FULLWIDTH P (JIS 2350)");

    /* ANK page: index = code, design packed in rows 0-7 (left byte). */
    expect(0, 0x39, 10, "ANK '9'");
    expect(0, 0x41, 10, "ANK 'A'");
    {
        int r, bad = 0;
        for (r = 8; r < 16; r++)
            if (p8_kanji_glyph[0][0x39][r * 2] || p8_kanji_glyph[0][0x39][r * 2 + 1])
                bad = 1;
        if (bad) { printf("FAIL ANK rows 8-15 not blank\n"); failures++; }
        else printf("ok   ANK rows 8-15 blank\n");
    }
    /* Space stays blank in both pages. */
    if (ink(0, 0x20) != 0) { printf("FAIL ANK space not blank\n"); failures++; }
    else printf("ok   ANK space blank\n");

    /* Level-2 kanji live in bank 1 (e.g. JIS 5424). */
    expect(1, ((((0x5424 & 0x0060) << 9) | ((0x5424 & 0x0F00) << 1) |
               ((0x5424 & 0x001F) << 4) | (0x5424 & 0x2000)) >> 4),
           10, "level-2 kanji (JIS 5424)");

    printf(failures ? "KANJIFONT TEST FAILED (%d)\n" : "KANJIFONT TEST PASSED\n",
           failures);
    return failures != 0;
}
