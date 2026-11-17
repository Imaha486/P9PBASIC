#!/usr/bin/env python3
"""Convert a PC-88 tokenized BASIC program (extracted from a D88 disk)
into P9PBASIC/P8PBASIC-loadable ASCII source (.BAS, Shift_JIS, CRLF).

Adaptations (P9PBASIC lacks these PC-88 features):
  * Labels: definitions `*NAME` at line start are dropped (the line keeps
    its number and any following statements); references after
    GOTO/GOSUB/RESTORE/THEN/ELSE become the definition's line number
    (THEN/ELSE gain an explicit GOTO).
  * STOP traps: `STOP ON` and `ON STOP GOSUB *x` statements are dropped
    (P9PBASIC cannot tokenize them). Occurrences are reported.
  * Driver extension statements FFDD/FFDE/FFDF become XDD/XDE/XDF
    (stub statements in the interpreter; see src/exec.c stmt_ext).

Usage: p88_to_bas.py <tokenized-file> <out.BAS>
The tokenized file is one file extracted by p88_extract.py.
Conversion notes/warnings go to stderr.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p88_detok import parse_lines, TOK, OPS, FUNC, mbf_single
import struct

def mbf_double(b):
    if len(b) < 8:
        return '?'
    exp = b[7]
    if exp == 0:
        return '0'
    sign = -1.0 if (b[6] & 0x80) else 1.0
    frac = (b[6] & 0x7F) << 48 | b[5] << 40 | b[4] << 32 | b[3] << 24 \
        | b[2] << 16 | b[1] << 8 | b[0]
    bits = frac | (1 << 55)
    return repr(sign * bits * (2.0 ** (exp - 128 - 55)))


EXT = {0xDD: 'XDD', 0xDE: 'XDE', 0xDF: 'XDF'}
# Converter's chosen interpretations for the two tentative functions.
FUNC = dict(FUNC)
FUNC[0x85] = 'INT'
FUNC[0x82] = 'RIGHT$'


def next_tok(body, i):
    """Index of the next non-space byte at/after i, or len(body)."""
    while i < len(body) and body[i] == 0x20:
        i += 1
    return i
NAMECH = set(b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789')


def read_name(body, i):
    j = i
    while j < len(body) and body[j] in NAMECH:
        j += 1
    return body[i:j].decode('ascii'), j


def collect_labels(lines):
    labels = {}
    for no, body in lines:
        if body[:1] == b'\xf5':
            name, _ = read_name(body, 1)
            if name:
                labels[name] = no
    return labels


def skip_stmt(body, i):
    """Skip raw bytes until ':' outside a string, or end of body."""
    instr = False
    while i < len(body):
        c = body[i]
        if instr:
            if c == 0x22:
                instr = False
        elif c == 0x22:
            instr = True
        elif c == 0x3A:
            break
        i += 1
    return i


def convert_body(body, labels, report, next_no):
    out = []
    i = 0
    n = len(body)
    stmt_start = True     # at start of a statement (for STOP-trap drops)
    expect_label = 0      # 1: after GOTO/GOSUB/RESTORE; 2: after THEN/ELSE
    in_data = False
    data_quote = False

    # label definition at line start
    if body[:1] == b'\xf5':
        name, j = read_name(body, 1)
        if name:
            i = j
            if i < n and body[i] == 0x3A:
                i += 1
            if i >= n:
                return 'REM *' + name
            stmt_start = True

    while i < n:
        b = body[i]
        if in_data and (data_quote or b != 0x3A):
            if b == 0x22:
                data_quote = not data_quote
            out.append(chr(b)); i += 1; continue
        if in_data and b == 0x3A:
            in_data = False  # fall through: ':' processed normally below
        if b == 0x22:  # string literal
            j = i + 1
            s = '"'
            while j < n and body[j] != 0x22:
                s += chr(body[j]); j += 1
            if j < n:
                s += '"'; j += 1
            out.append(s); i = j
            expect_label = 0; stmt_start = False
            continue
        if b == 0x8F and i + 1 < n and body[i + 1] == 0xE9:  # ' comment
            out.append("'" + ''.join(chr(c) for c in body[i + 2:]))
            i = n
            continue
        if b == 0xFF:
            t = body[i + 1] if i + 1 < n else 0
            if t in EXT:
                out.append(EXT[t] + ' ')
            elif t in FUNC:
                out.append(FUNC[t])
            else:
                out.append(f'<FF{t:02X}>')
                report.append(f'unknown FF token FF{t:02X}')
            i += 2
            expect_label = 0; stmt_start = False
            continue
        # STOP trap drops (statement start only; tokens may be space-separated)
        if stmt_start and b == 0x90:
            j = next_tok(body, i + 1)
            if j < n and body[j] == 0x95:
                report.append('dropped STOP ON')
                i = j + 1
                continue  # still at statement start (now at ':' or next stmt)
        if stmt_start and b == 0x95:
            j = next_tok(body, i + 1)
            k = next_tok(body, j + 1) if j < n else n
            if j < n and body[j] == 0x90 and k < n and body[k] == 0x8D:
                report.append('dropped ON STOP GOSUB')
                i = skip_stmt(body, k + 1)
                continue
        if b in TOK:
            name = TOK[b]
            out.append(name + (' ' if name[-1].isalnum() else ''))
            if b == 0x84:
                in_data = True
            if b in (0x89, 0x8D, 0x8C):
                expect_label = 1
            elif b in (0xDD, 0x9F):
                expect_label = 2
            else:
                expect_label = 0
            stmt_start = (b == 0xDD or b == 0x9F)  # THEN/ELSE start a stmt
            i += 1
            continue
        if b == 0xF5 and expect_label:
            name, j = read_name(body, i + 1)
            if name and name in labels:
                if expect_label == 2:
                    out.append('GOTO ')
                out.append(str(labels[name]))
                i = j
                expect_label = 0; stmt_start = False
                continue
            # Unresolved in the shipped program (e.g. MENU.ALP's *TYPE):
            # fall through to the next line and report loudly.
            report.append(f'UNRESOLVED label *{name} -> falls through to {next_no}')
            if expect_label == 2:
                out.append('GOTO ')
            out.append(str(next_no))
            i = j if name else i + 1
            expect_label = 0; stmt_start = False
            continue
        if b in OPS:
            op = OPS[b]
            out.append(f' {op} ' if op.isalpha() else op)
            expect_label = 0; stmt_start = False
            i += 1
            continue
        if b == 0x0B:
            out.append('&O' + str(struct.unpack_from('<H', body, i + 1)[0])); i += 3
        elif b == 0x0C:
            out.append('&H' + format(struct.unpack_from('<H', body, i + 1)[0], 'X')); i += 3
        elif b == 0x0E:
            out.append(str(struct.unpack_from('<H', body, i + 1)[0])); i += 3
        elif b == 0x0F:
            out.append(str(body[i + 1])); i += 2
        elif 0x11 <= b <= 0x1A:
            out.append(str(b - 0x11)); i += 1
        elif b == 0x1C:
            out.append(str(struct.unpack_from('<h', body, i + 1)[0])); i += 3
        elif b == 0x1D:
            out.append(mbf_single(body[i + 1:i + 5])); i += 5
        elif b == 0x1F:
            out.append(mbf_double(body[i + 1:i + 9])); i += 9
        elif b == 0x3A:
            out.append(':'); i += 1
            stmt_start = True; expect_label = 0
        elif 0x20 <= b < 0x7F:
            out.append(chr(b)); i += 1
            if b != 0x20:
                expect_label = 0; stmt_start = False
        else:
            out.append(f'<{b:02X}>')
            report.append(f'unknown byte {b:02X}')
            i += 1
    s = ''.join(out).strip().strip(':').strip()
    return s if s else 'REM'


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    data = open(sys.argv[1], 'rb').read()
    lines = parse_lines(data)
    labels = collect_labels(lines)
    print(f'labels: {labels}', file=sys.stderr)
    out_lines = []
    for idx, (no, body) in enumerate(lines):
        next_no = lines[idx + 1][0] if idx + 1 < len(lines) else no
        report = []
        text = convert_body(body, labels, report, next_no)
        for r in report:
            print(f'line {no}: {r}', file=sys.stderr)
        out_lines.append(f'{no} {text}')
    blob = ('\r\n'.join(out_lines) + '\r\n').encode('latin-1')
    open(sys.argv[2], 'wb').write(blob)
    print(f'wrote {sys.argv[2]}: {len(out_lines)} lines, {len(blob)} bytes',
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
