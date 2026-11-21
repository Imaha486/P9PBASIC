/* Kanji font table checks.
 *
 * The fullwidth indices below were verified against MA2026's actual
 * kanji-ROM port traffic (the guest latches these addresses when it
 * plots the panel text) and the NEC ROM address map described by the
 * QUASI88 emulator (kanji2addr):
 *   "PC-88... MUSIC ARENA 2026 ... PART2" header and song comments.
 *
 * Single-byte (ANK) glyphs use the genuine ROM's FLAT layout: glyph
 * for code C occupies 16 consecutive stream bytes at offset C*16
 * (the stream is p8_kanji_glyph flattened in [idx][row*2+half]
 * order). MA2026's XDD text plotter fetches them at addr = C*8,
 * reading (E9,E8) pairs — stream bytes (C*8 + r)*2 + {0,1} — which
 * is the glyph's 16 rows in order; QUASI88's status renderer reads
 * ANK glyphs the same way (&kanji_rom[0][c * 8][0], one stream byte
 * per row). An earlier index = code placement rendered XDD ASCII
 * text ("Presented by MEGAFEPPS,2026") as mojibake.
 *
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

static int stream_ink(int code)
{
    /* Ink of the 16 stream bytes at offset code*16 (bank 0). */
    int n = 0, j;
    for (j = 0; j < 16; j++) {
        int s = code * 16 + j;
        unsigned char b = p8_kanji_glyph[0][s >> 5][s & 31];
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

static void expect_stream(int code, int min_ink, const char *what)
{
    int n = stream_ink(code);
    if (n < min_ink) {
        printf("FAIL %s: ANK code 0x%02X stream ink %d < %d\n",
               what, code, n, min_ink);
        failures++;
    } else {
        printf("ok   %s: ANK code 0x%02X stream ink %d\n", what, code, n);
    }
}

int main(void)
{
    /* Trace-verified fullwidth placements (MA2026 panel text). */
    expect(0, 0x26D, 30, "FULLWIDTH M (JIS 234D)");
    expect(0, 0x65B, 40, "ON/oto kanji (JIS 323B)");
    expect(0, 0x13C, 5,  "long-vowel mark (JIS 213C)");
    expect(0, 0x1B7, 10, "katakana SHI (JIS 2537)");
    expect(0, 0x7B8, 50, "SHUU/atsumu kanji (JIS 3D38)");
    expect(0, 0x270, 20, "FULLWIDTH P (JIS 2350)");

    /* ANK flat layout: the XDD fetch sequence for "Presented..."
     * reads stream bytes at code*16. */
    expect_stream(0x50, 15, "ANK 'P'");
    expect_stream(0x39, 15, "ANK '9'");
    expect_stream(0x41, 10, "ANK 'A'");
    expect_stream(0x72, 10, "ANK 'r'");
    /* Halfwidth kana also live in the flat page (JIS X 0201). */
    expect_stream(0xB6, 5, "halfwidth KA (0xB6)");

    /* Space stays blank in the flat page. */
    if (stream_ink(0x20) != 0) { printf("FAIL ANK space not blank\n"); failures++; }
    else printf("ok   ANK space blank\n");

    /* Level-2 kanji live in bank 1 (e.g. JIS 5424). */
    expect(1, ((((0x5424 & 0x0060) << 9) | ((0x5424 & 0x0F00) << 1) |
               ((0x5424 & 0x001F) << 4) | (0x5424 & 0x2000)) >> 4),
           10, "level-2 kanji (JIS 5424)");

    printf(failures ? "KANJIFONT TEST FAILED (%d)\n" : "KANJIFONT TEST PASSED\n",
           failures);
    return failures != 0;
}
