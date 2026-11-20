#!/usr/bin/env python3
"""Generate src/kanjifont.c — the substitute PC-88 kanji/ANK font table.

Glyph data: Shinonome Font (東雲フォント), (c) /efont/ Project, PUBLIC
DOMAIN (free modification, format conversion, embedding, redistribution).
  - shnmk16.bdf   : Gothic 16x16, JIS X 0208 (6879 glyphs)
  - shnm8x16r.bdf : Gothic 8x16, JIS X 0201 (ANK + halfwidth kana)

Layout: the NEC kanji ROM address map, as described by the QUASI88
open-source emulator (src/q8tk-glib.cpp, kanji2addr) — a published
hardware-behaviour description. For a JIS code (J1<<8)|J2:

  J1 < 0x30 (non-kanji)      bank 0
    addr = ((code & 0x0060) << 7) | ((code & 0x0700) << 1)
         | ((code & 0x001F) << 4)
  0x30 <= J1 < 0x50 (level 1) bank 0
    addr = ((code & 0x0060) << 9) | ((code & 0x1F00) << 1)
         | ((code & 0x001F) << 4)
  0x50 <= J1 < 0x70 (level 2a) bank 1
    addr = ((code & 0x0060) << 9) | ((code & 0x0F00) << 1)
         | ((code & 0x001F) << 4) | (code & 0x2000)
  J1 >= 0x70 (level 2b)       bank 1
    addr = ((code & 0x0060) << 7) | ((code & 0x0700) << 1)
         | ((code & 0x001F) << 4)

  glyph index = addr >> 4; each glyph is 16 rows x 2 bytes [left, right],
  matching the port protocol (IN E9 = byte[0] = left, IN E8 = byte[1]).

Single-byte characters (JIS X 0201: ASCII + halfwidth kana) are laid
out FLAT, as in the genuine ROM: glyph for code C occupies 16
consecutive stream bytes at offset C*16 (the byte stream is
p8_kanji_glyph flattened in [idx][row*2+half] order). The guest
fetches them through the kanji ports at addr = C*8, reading
(E9,E8) pairs — i.e. stream bytes (C*8 + r)*2 + {0,1} for r = 0..7,
which is exactly the glyph's 16 rows in order. (QUASI88's own status
renderer reads ANK glyphs the same way: &kanji_rom[0][c * 8][0],
advancing one stream byte per row, 16 rows.) An earlier layout that
placed ANK glyphs at index = code rendered XDD-plotted ASCII text
(e.g. MA2026's "Presented by MEGAFEPPS,2026") as mojibake.

Usage: gen_kanjifont.py <shnmk16.bdf> <shnm8x16r.bdf> <out.c>
"""
import sys


def parse_bdf(path):
    glyphs = {}
    enc = None
    rows = None
    in_bitmap = False
    with open(path, encoding="ascii", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("ENCODING"):
                enc = int(line.split()[1])
                rows = []
            elif line == "BITMAP":
                in_bitmap = True
            elif line == "ENDCHAR":
                if enc is not None and rows is not None:
                    glyphs[enc] = rows
                enc = None
                rows = None
                in_bitmap = False
            elif in_bitmap:
                rows.append(int(line, 16))
    return glyphs


def kanji_addr(code):
    """(bank, addr) for a JIS X 0208 code, per the NEC ROM layout."""
    j1 = code >> 8
    if j1 < 0x30:
        return 0, ((code & 0x0060) << 7) | ((code & 0x0700) << 1) | \
                  ((code & 0x001F) << 4)
    if j1 < 0x50:
        return 0, ((code & 0x0060) << 9) | ((code & 0x1F00) << 1) | \
                  ((code & 0x001F) << 4)
    if j1 < 0x70:
        return 1, ((code & 0x0060) << 9) | ((code & 0x0F00) << 1) | \
                  ((code & 0x001F) << 4) | (code & 0x2000)
    return 1, ((code & 0x0060) << 7) | ((code & 0x0700) << 1) | \
              ((code & 0x001F) << 4)


def main():
    full_bdf, half_bdf, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    table = [[[0] * 32 for _ in range(4096)] for _ in range(2)]
    collisions = []

    full = parse_bdf(full_bdf)
    placed_full = 0
    for code, rows in sorted(full.items()):
        if len(rows) != 16:
            continue
        bank, addr = kanji_addr(code)
        idx = addr >> 4
        glyph = []
        for r in rows:  # 16 bits per row, MSB = leftmost pixel
            glyph.append((r >> 8) & 0xFF)
            glyph.append(r & 0xFF)
        if any(table[bank][idx]) and table[bank][idx] != glyph:
            collisions.append((code, bank, idx))
        table[bank][idx] = glyph
        placed_full += 1

    half = parse_bdf(half_bdf)
    placed_half = 0
    ank_collisions = []
    for code, rows in sorted(half.items()):
        if not (0 <= code <= 0xFF) or len(rows) != 16:
            continue
        for j in range(16):  # flat: stream byte C*16 + j = glyph row j
            s = code * 16 + j
            idx, byte = s >> 5, s & 31
            if table[0][idx][byte] and table[0][idx][byte] != rows[j]:
                ank_collisions.append((code, j))
            table[0][idx][byte] = rows[j] & 0xFF
        placed_half += 1
    if ank_collisions:
        print(f"ANK flat collisions: {len(ank_collisions)} {ank_collisions[:5]}")

    with open(out_path, "w") as f:
        f.write("/* Substitute kanji/ANK font for the PC-88 kanji ROM ports.\n"
                " *\n"
                " * Glyph data: Shinonome Font (public domain; see\n"
                " * tools/gen_kanjifont.py). Layout: NEC kanji ROM address\n"
                " * map as described by the QUASI88 emulator (kanji2addr).\n"
                " * p8_kanji_glyph[bank][index][row * 2 + half],\n"
                " * half 0 = left byte (IN E9), half 1 = right byte (IN E8).\n"
                " * Generated file — do not edit by hand.\n"
                " */\n")
        f.write("const unsigned char p8_kanji_glyph[2][4096][32] = {\n")
        for bank in range(2):
            f.write("  {\n")
            for idx in range(4096):
                f.write("    {" + ",".join(str(b) for b in table[bank][idx]) + "},\n")
            f.write("  },\n")
        f.write("};\n")

    print(f"fullwidth placed: {placed_full}, halfwidth placed: {placed_half}")
    print(f"collisions (same slot, different glyph): {len(collisions)}")
    for code, bank, idx in collisions[:10]:
        print(f"  JIS {code:04X} -> bank {bank} idx {idx:#05x}")
    # spot checks against port-trace-verified indices (MA2026 panel)
    checks = {"Ｍ": 0x26D, "音": 0x65B, "ー": 0x13C, "シ": 0x1B7, "集": 0x7B8}
    for ch, want in checks.items():
        j1, j2 = (b - 0x80 for b in ch.encode("euc_jp"))
        bank, addr = kanji_addr((j1 << 8) | j2)
        got = addr >> 4
        ink = sum(bin(b).count("1") for b in table[bank][got])
        print(f"  check {ch} JIS {(j1 << 8) | j2:04X}: idx {got:#05x} "
              f"(want {want:#05x}) ink {ink} {'OK' if got == want and ink else 'MISMATCH'}")
    s9 = [table[0][(0x39 * 16 + j) >> 5][(0x39 * 16 + j) & 31] for j in range(16)]
    ink9 = sum(bin(b).count("1") for b in s9)
    print(f"  check ANK '9' flat stream @0x390: ink {ink9} {'OK' if ink9 else 'MISSING'}")
    sp = [table[0][(0x50 * 16 + j) >> 5][(0x50 * 16 + j) & 31] for j in range(16)]
    inkp = sum(bin(b).count("1") for b in sp)
    print(f"  check ANK 'P' flat stream @0x500: ink {inkp} {'OK' if inkp else 'MISSING'}")


if __name__ == "__main__":
    main()
