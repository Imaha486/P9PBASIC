#!/usr/bin/env python3
"""Convert a PC-88 tokenized BASIC program (extracted from a D88 disk)
into P9PBASIC/P8PBASIC-loadable ASCII source (.BAS, Shift_JIS, CRLF).

Labels pass through unchanged: `*NAME` definitions and references are
emitted as written and resolved by the interpreter's own label support.
A reference to a label the program does not define is reported and still
passed through (the interpreter raises "undefined line" if it is ever
executed, as the PC-88 does). Names are read and matched by the
interpreter's rules (see the note above read_run). A keyword token that
directly follows a label gets a separator (':' after a definition, a
space after a reference) so the interpreter does not read it into the
name.

Numeric constants are spelled so the interpreter reads back the token's
type and exact value, negative ones in parentheses (p88_detok.py, note
above num_literal).

Adaptations (the interpreter lacks these PC-88 features):
  * STOP traps: `STOP ON` and `ON STOP GOSUB *x` statements are dropped
    and reported (documented approximation: docs/APPROXIMATIONS.md
    item 26; the interpreter has no STOP trap, and `STOP ON` passed
    through would execute as a plain STOP).
  * Driver extension statements FFDD/FFDE/FFDF become XDD/XDE/XDF
    (stub statements in the interpreter; see src/exec.c stmt_ext).

Usage: p88_to_bas.py <tokenized-file> <out.BAS>
The tokenized file is one file extracted by p88_extract.py.
Conversion notes/warnings go to stderr. An input that is not a well-formed
tokenized program (p88_detok.parse_lines validates the line chain) is
rejected with one error line on stderr and exit status 1.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p88_detok import (parse_lines, TOK, OPS, FUNC, mbf_single, mbf_double,
                       int_literal, need)
import struct


EXT = {0xDD: 'XDD', 0xDE: 'XDE', 0xDF: 'XDF'}
# Converter's chosen interpretation for the one tentative function.
FUNC = dict(FUNC)
FUNC[0x85] = 'INT'


def next_tok(body, i):
    """Index of the next non-space byte at/after i, or len(body)."""
    while i < len(body) and body[i] == 0x20:
        i += 1
    return i


# Label names follow the interpreter's grammar, because the interpreter is
# what resolves them in the converted text:
#   - a name is a letter followed by letters, digits and '.' (src/token.c
#     label scan, src/exec.c label_char); `*100` is not a label there
#     (GOTO *100 is a syntax error);
#   - the tokenizer upper-cases definitions and references alike, so
#     lookup ignores case;
#   - a definition is `*NAME` as the first thing on a line, leading spaces
#     allowed (find_label); the whole name must match (*SUB does not find
#     *SUB.1), and the first definition of a name wins.
ALPHA = set(b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz')
LABCH = ALPHA | set(b'0123456789.')


def read_run(body, i):
    """The run of label characters at body[i:] -> (text, end index)."""
    j = i
    while j < len(body) and body[j] in LABCH:
        j += 1
    return body[i:j].decode('ascii'), j


def read_label(body, i):
    """A label name at body[i:] -> (name, end index); ('', i) when what
    is there does not start with a letter."""
    if i >= len(body) or body[i] not in ALPHA:
        return '', i
    return read_run(body, i)


def label_def(body):
    """The label a line body defines -> (name, end index), or ('', 0)."""
    i = next_tok(body, 0)
    if body[i:i + 1] == b'\xf5':
        return read_label(body, i + 1)
    return '', 0


def glues(body, i):
    """True when body[i] is a keyword token: its text starts with a letter
    and would be read by the interpreter's tokenizer as more of a label
    name written directly before it (`*ABC` + PRINT -> label ABCPRINT)."""
    if i >= len(body):
        return False
    b = body[i]
    if b == 0xFF:
        t = body[i + 1] if i + 1 < len(body) else 0
        return t in EXT or t in FUNC
    return b in TOK and TOK[b][0].isalnum()


def collect_labels(lines):
    """{folded name: line number} of the labels the interpreter can find."""
    labels = {}
    for no, body in lines:
        name, _ = label_def(body)
        if name:
            labels.setdefault(name.upper(), no)
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


def convert_body(body, labels, report):
    out = []
    i = 0
    n = len(body)
    stmt_start = True     # at start of a statement (for STOP-trap drops)
    expect_label = False  # after GOTO/GOSUB/RESTORE/THEN/ELSE
    in_data = False
    data_quote = False

    # label definition at line start: pass "*NAME" through. What follows
    # is still the start of a statement, with or without a ':'. A keyword
    # token directly after the name gets the ':' the interpreter's
    # tokenizer needs to end the name there (`*NAME:STATEMENT`, the form
    # MA2026 itself uses); a definition already followed by ':' or a space
    # is left as written.
    name, j = label_def(body)
    if name:
        out.append('*' + name)
        if glues(body, j):
            out.append(':')
        i = j

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
            expect_label = b in (0x89, 0x8D, 0x8C, 0xDD, 0x9F)
            stmt_start = (b == 0xDD or b == 0x9F)  # THEN/ELSE start a stmt
            i += 1
            continue
        # F5 in GOTO/GOSUB/RESTORE/THEN/ELSE position is a label ref:
        # pass "*NAME" through. One the program does not define (e.g.
        # MENU.ALP's *TYPE) is reported; the interpreter raises
        # "undefined line" if that statement is ever executed. A name
        # that does not start with a letter (`*100`) is not a label to the
        # interpreter at all (syntax error) and is reported the same way.
        # A keyword token directly after the name is set off with a space
        # so the tokenizer does not read it into the name. (F5 with no
        # name after it is left to the operator case below.)
        if b == 0xF5 and expect_label:
            name, j = read_run(body, i + 1)
            if name:
                out.append('*' + name)
                if ord(name[0]) not in ALPHA or name.upper() not in labels:
                    report.append(f'UNDEFINED label *{name} (passed through)')
                if glues(body, j):
                    out.append(' ')
                i = j
                stmt_start = False  # expect_label stays for "*A,*B"
                continue
        if b in OPS:
            op = OPS[b]
            out.append(f' {op} ' if op.isalpha() else op)
            expect_label = 0; stmt_start = False
            i += 1
            continue
        # Number tokens. A payload cut off by the end of the body is a
        # framing error, not a program (p88conv.c returns -1 here).
        if b in (0x0B, 0x0C, 0x0E, 0x1C):
            need(body, i, 2)
        elif b == 0x0F:
            need(body, i, 1)
        elif b == 0x1D:
            need(body, i, 4)
        elif b == 0x1F:
            need(body, i, 8)
        if b == 0x0B:
            out.append('&O' + format(struct.unpack_from('<H', body, i + 1)[0], 'o')); i += 3
        elif b == 0x0C:
            out.append('&H' + format(struct.unpack_from('<H', body, i + 1)[0], 'X')); i += 3
        elif b == 0x0E:
            out.append(str(struct.unpack_from('<H', body, i + 1)[0])); i += 3
        elif b == 0x0F:
            out.append(str(body[i + 1])); i += 2
        elif 0x11 <= b <= 0x1A:
            out.append(str(b - 0x11)); i += 1
        elif b == 0x1C:
            out.append(int_literal(struct.unpack_from('<h', body, i + 1)[0])); i += 3
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
                # ',' keeps the label position: ON x GOSUB *A,*B
                if b != 0x2C:
                    expect_label = False
                stmt_start = False
        else:
            out.append(f'<{b:02X}>')
            report.append(f'unknown byte {b:02X}')
            i += 1
    s = ''.join(out).strip().strip(':').strip()
    return s if s else 'REM'


def convert(data, log=None):
    """Tokenized program bytes -> .BAS file bytes (what p88_convert()
    in src/p88conv.c returns). Report lines go to log(str) when given.
    Raises ValueError when data is not a well-formed tokenized program
    (broken line chain, truncated token); p88_convert() returns NULL for
    the same inputs."""
    lines = parse_lines(data)
    labels = collect_labels(lines)
    if log:
        log(f'labels: {labels}')
    out_lines = []
    for no, body in lines:
        report = []
        try:
            text = convert_body(body, labels, report)
        except ValueError as e:
            raise ValueError(f'line {no}: {e}') from None
        if log:
            for r in report:
                log(f'line {no}: {r}')
        out_lines.append(f'{no} {text}')
    return ('\r\n'.join(out_lines) + '\r\n').encode('latin-1'), len(out_lines)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    # The report is held back until the conversion has succeeded, so a
    # rejected input produces exactly one line on stderr and no output file.
    notes = []
    try:
        data = open(sys.argv[1], 'rb').read()
        blob, nlines = convert(data, notes.append)
    except (OSError, ValueError) as e:
        print(f'p88_to_bas.py: {sys.argv[1]}: {e}', file=sys.stderr)
        return 1
    for s in notes:
        print(s, file=sys.stderr)
    open(sys.argv[2], 'wb').write(blob)
    print(f'wrote {sys.argv[2]}: {nlines} lines, {len(blob)} bytes',
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
