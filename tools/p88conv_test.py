#!/usr/bin/env python3
"""Unit checks for the PC-88 token -> text conversion (review items T-4/T-5).

Usage: p88conv_test.py <p88conv-binary> <outdir> [<p8pbasic-binary>]

<p88conv-binary> is src/p88conv.c built with -DP88CONV_TEST. Every check
runs against the three implementations that must agree:
  tools/p88_to_bas.py (convert), tools/p88_detok.py (mbf_* / detok) and
  the C converter (byte-for-byte equal to the Python converter's output).

<p8pbasic-binary> is the host interpreter (build-host/p8pbasic). With it
the converted programs are also RUN, and what they print is checked: the
text of a constant only matters through what the interpreter makes of it.
Without it those checks are skipped (and said so); tests/run_regress.sh
always passes it.

Checked:
  - Numeric constants (MBF single / double, int16, &O octal) convert to
    the exact expected text, C and Python alike: type-preserving
    spelling ('!' / '#' suffix, E / D exponent, the product form above
    the single literal limit), parenthesized negatives, Python's
    shortest round-trip digits (2^-24 parity payload), and the
    documented 56 -> 53 bit rounding of MBF doubles.
  - CONVEVAL: the same kinds of constants inside PRINT expressions whose
    printed result depends on the type and value the interpreter read
    back: (x+1)-x is 0 for a single 16777216 and 1 for a double;
    (-2.5)^2 is 6.25; a type probe that prints 0 for a single and 1 for
    a double.
  - CONVFUNC: the FF function tokens FF83 (MID$) and FFDC (TIME$) as
    FC88V42 MENU.ALP line 286 uses them convert to
    TM=VAL(MID$(TIME$,8,1)) with no "unknown FF token" report, in the
    detokenizer and both converters; the host runs that statement and
    prints only clock-independent results.
  - Line framing: the last line of a file is a statement ending in a
    constant whose payload holds 00 bytes; files that end with zero
    padding, with the bare 00 00 end marker, or right after the last
    terminator all convert to the same text.
  - CONVLAB: a program using labels keeps them (`GOSUB *TYPE`,
    `THEN *OK`, the `*TYPE` / `*OK` definitions), an undefined label is
    passed through and reported, and STOP ON / ON STOP GOSUB are dropped
    and reported (docs/APPROXIMATIONS.md item 26).
  - CONVLAB2: label rules follow the interpreter: a keyword token
    directly after a definition or reference is set off (':' / space)
    and the statement runs, references fold case, dotted names match
    whole, leading spaces do not hide a definition, digit-leading names
    are not labels; the warning fires for exactly the references the
    interpreter cannot resolve.
The converted label program is left in <outdir>/CONVLAB.BAS; the caller
(tests/run_regress.sh) runs it in the host interpreter and checks that it
prints "labrun 1".

There is no text -> PC-88 token encoder in the tree, so there is no
round-trip check. Exit 0 = all checks pass.
"""
import math
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p88_detok
import p88_to_bas

fails = 0


def chk(what, got, want):
    global fails
    if got != want:
        print(f'FAIL {what}: got {got!r} want {want!r}')
        fails += 1


# What follows the last line's 00 terminator in a file.
PADDED = b'\x00' * 16       # end marker + zero padding (sector-padded file)
MARKER = b'\x00\x00'        # end marker only (exact-length file)
CUT = b''                   # file cut right after the terminator


def program(lines, tail=PADDED):
    """Frame (lineno, body) pairs as a tokenized file:
    [link u16][lineno u16][body][00] per line, link = file offset of the
    next line (for the last line: of the 00 00 end marker), then tail."""
    out = bytearray()
    for no, body in lines:
        nxt = len(out) + 4 + len(body) + 1
        out += bytes([nxt & 0xFF, nxt >> 8, no & 0xFF, no >> 8]) + body + b'\x00'
    return bytes(out) + tail


# ---- MBF payloads (mantissa little-endian, sign in the top mantissa bit,
# exponent last; 1.0 = exponent 0x81 with a zero stored mantissa).

def mbf_s(x):
    """MBF single payload of a float that fits 24 mantissa bits."""
    m, e = math.frexp(abs(x))           # abs(x) = m * 2**e, 0.5 <= m < 1
    bits = int(m * (1 << 24))
    assert bits == m * (1 << 24) and 0 < e + 128 < 256
    bits &= 0x7FFFFF
    if x < 0:
        bits |= 0x800000
    return bits.to_bytes(3, 'little') + bytes([e + 128])


def mbf_d(x):
    """MBF double payload of a float (53 of the 56 mantissa bits used)."""
    m, e = math.frexp(abs(x))
    bits = int(m * (1 << 56))
    assert bits == m * (1 << 56) and 0 < e + 128 < 256
    bits &= (1 << 55) - 1
    if x < 0:
        bits |= 1 << 55
    return bits.to_bytes(7, 'little') + bytes([e + 128])


S_1_0 = bytes([0x00, 0x00, 0x00, 0x81])
S_0_5 = bytes([0x00, 0x00, 0x00, 0x80])
S_M2_5 = bytes([0x00, 0x00, 0xA0, 0x82])
S_0_1 = bytes([0xCD, 0xCC, 0x4C, 0x7D])      # float32 nearest to 0.1
S_2P24 = bytes([0x00, 0x00, 0x00, 0x99])     # 16777216
S_2M24 = bytes([0x00, 0x00, 0x00, 0x69])     # 2^-24 (C/Python parity case)
S_MAX = bytes([0xFF, 0xFF, 0x7F, 0xFF])      # largest MBF single
S_HALFMAX = bytes([0xFF, 0xFF, 0x7F, 0xFE])  # half of it
S_ZERO = bytes(4)
D_1_0 = bytes([0, 0, 0, 0, 0, 0, 0x00, 0x81])
D_0_5 = bytes([0, 0, 0, 0, 0, 0, 0x00, 0x80])
D_M2_5 = bytes([0, 0, 0, 0, 0, 0, 0xA0, 0x82])
D_2P24 = bytes([0, 0, 0, 0, 0, 0, 0x00, 0x99])
D_1EM5 = mbf_d(1e-05)
D_1P30 = bytes([0, 0, 0, 0x02, 0, 0, 0x00, 0x81])   # 1 + 2^-30
D_1P55 = bytes([0x01, 0, 0, 0, 0, 0, 0x00, 0x81])   # 1 + 2^-55 (56-bit only)
D_ZERO = bytes(8)

# Expected spellings (see the note above num_literal in p88_detok.py).
T_S_0_1 = '0.10000000149011612!'            # > 7 digits: needs '!'
T_S_2P24 = '16777216.0!'
T_S_2M24 = '5.960464477539063E-08'
T_S_HALFMAX = '8.5070586659632215E+37'
T_S_MAX = '(' + T_S_HALFMAX + '*2)'         # above the literal limit
T_D_2P24 = '16777216.0'                     # > 7 digits: double as is
T_D_1EM5 = '1D-05'
T_D_1P30 = '1.0000000009313226'

# ---- token helpers
PRINT, EQ, PLUS, MINUS, MUL, DIV, POW = (b'\x91', b'\xf1', b'\xf3', b'\xf4',
                                         b'\xf5', b'\xf6', b'\xf7')


def dig(n):
    return bytes([0x11 + n])                # digit tokens 0-9


def i16(v):
    return b'\x1c' + struct.pack('<h', v)   # int16 token


def sng(payload):
    return b'\x1d' + payload


def dbl(payload):
    return b'\x1f' + payload


# ---- text-only fixtures: (line number, body, expected text)
NUM_LINES = [
    (10, b'A' + EQ + sng(S_1_0), b'A=1.0'),
    (20, b'A' + EQ + sng(S_0_5), b'A=0.5'),
    (30, b'A' + EQ + sng(S_M2_5), b'A=(-2.5)'),
    (35, b'A' + EQ + sng(S_0_1), b'A=' + T_S_0_1.encode()),
    (40, b'B#' + EQ + dbl(D_1_0), b'B#=1.0#'),
    (50, b'B#' + EQ + dbl(D_0_5), b'B#=0.5#'),
    (60, b'B#' + EQ + dbl(D_M2_5), b'B#=(-2.5#)'),
    (70, b'C' + EQ + b'\x0b\x0f\x00', b'C=&O17'),
    (80, b'A' + EQ + sng(S_2P24), b'A=' + T_S_2P24.encode()),
    (90, b'B#' + EQ + dbl(D_2P24), b'B#=' + T_D_2P24.encode()),
    (100, b'B#' + EQ + dbl(D_1EM5), b'B#=' + T_D_1EM5.encode()),
    (110, b'A' + EQ + sng(S_2M24), b'A=' + T_S_2M24.encode()),
    (120, b'A' + EQ + sng(S_MAX), b'A=' + T_S_MAX.encode()),
    (130, b'A' + EQ + sng(bytes([0xFF, 0xFF, 0xFF, 0xFF])),
     b'A=(-' + T_S_MAX.encode() + b')'),
    (140, b'A' + EQ + sng(S_ZERO), b'A=0!'),
    (150, b'B#' + EQ + dbl(D_ZERO), b'B#=0#'),
    (160, b'C' + EQ + i16(-5), b'C=(-5)'),
    (170, b'C' + EQ + i16(-32768), b'C=(-32767-1)'),
    (180, b'C' + EQ + i16(300), b'C=300'),
    (190, b'B#' + EQ + dbl(D_1P30), b'B#=' + T_D_1P30.encode()),
    # documented limit: the 56th mantissa bit does not fit a binary64
    (200, b'B#' + EQ + dbl(D_1P55), b'B#=1.0#'),
    # last line: a statement ending in a constant with 00 payload bytes
    (210, b'A' + EQ + sng(S_1_0), b'A=1.0'),
]

# Last line ends in a line-number reference whose high byte is 00.
END_LINES = [
    (10, b'A' + EQ + dig(1), b'A=1'),
    (20, b'\x89\x0e\x0a\x00', b'GOTO 10'),
]


# ---- CONVEVAL: constants evaluated by the host interpreter.
# (line number, body, expected text, expected screen row)

def probe(lit):
    """Tokens of the type probe (L*0+4096)*4096+1-(L*0+4096)*4096.
    L*0+4096 is 4096 in L's type, so the first product is 2^24 in that
    type; adding 1 is lost in a single (prints 0) and kept in a double
    (prints 1)."""
    base = b'(' + lit + MUL + dig(0) + PLUS + i16(4096) + b')' + MUL + i16(4096)
    return base + PLUS + dig(1) + MINUS + base


def probe_text(lit):
    base = f'({lit}*0+4096)*4096'
    return f'PRINT {base}+1-{base}'.encode()


def ev_probe(no, lit, text, row):
    return (no, PRINT + probe(lit), probe_text(text), row)


EVAL_LINES = [
    # single 16777216: (x+1)-x = 0; read as a double it would be 1
    (10, PRINT + b'(' + sng(S_2P24) + PLUS + dig(1) + b')' + MINUS + sng(S_2P24),
     f'PRINT ({T_S_2P24}+1)-{T_S_2P24}'.encode(), ' 0'),
    (20, PRINT + b'(' + dbl(D_2P24) + PLUS + dig(1) + b')' + MINUS + dbl(D_2P24),
     f'PRINT ({T_D_2P24}+1)-{T_D_2P24}'.encode(), ' 1'),
    # negative constant as the base of ^: 6.25, not -6.25
    (30, PRINT + sng(S_M2_5) + POW + dig(2), b'PRINT (-2.5)^2', ' 6.25'),
    (40, PRINT + dbl(D_M2_5) + POW + dig(2), b'PRINT (-2.5#)^2', ' 6.25'),
    (50, PRINT + i16(-5) + POW + dig(2), b'PRINT (-5)^2', ' 25'),
    # type probes: single -> 0, double -> 1
    ev_probe(60, sng(S_1_0), '1.0', ' 0'),
    ev_probe(70, dbl(D_1_0), '1.0#', ' 1'),
    ev_probe(80, sng(S_0_1), T_S_0_1, ' 0'),
    ev_probe(90, dbl(D_1EM5), T_D_1EM5, ' 1'),
    ev_probe(100, sng(S_2M24), T_S_2M24, ' 0'),
    ev_probe(110, sng(S_MAX), T_S_MAX, ' 0'),
    ev_probe(120, sng(S_ZERO), '0!', ' 0'),
    ev_probe(130, dbl(D_ZERO), '0#', ' 1'),
    # the largest single is read back without Overflow and with its
    # exact value: half of it equals the constant one exponent lower
    (140, PRINT + sng(S_MAX) + DIV + dig(2) + EQ + sng(S_HALFMAX),
     f'PRINT {T_S_MAX}/2={T_S_HALFMAX}'.encode(), '-1'),
    # a double keeps the bits a single would drop: (1+2^-30-1)*2^30 = 1
    (150, PRINT + b'(' + dbl(D_1P30) + MINUS + dig(1) + b')' + MUL + i16(16384)
     + MUL + i16(16384) + MUL + dig(4),
     f'PRINT ({T_D_1P30}-1)*16384*16384*4'.encode(), ' 1'),
    # last line: a statement ending in the 1.0 constant, nothing after it
    (160, PRINT + dig(1) + PLUS + sng(S_1_0), b'PRINT 1+1.0', ' 2'),
]

# ---- CONVFUNC: FF83 = MID$ and FFDC = TIME$ (FC88V42 MENU.ALP line 286).
# (line number, body, expected text); FUNC_ROWS is what the host prints.
# TIME$ itself is clock-dependent, so only its shape is printed: line 10
# is the real statement (it must run), line 30 prints the length of the
# same substring, line 20 the value from a literal time string.
FF_LEN, FF_VAL, FF_MID, FF_TIME = b'\xff\x92', b'\xff\x94', b'\xff\x83', b'\xff\xdc'
MID_TIME = FF_MID + b'(' + FF_TIME + b',' + dig(8) + b',' + dig(1) + b')'
FUNC_LINES = [
    (10, b'TM' + EQ + FF_VAL + b'(' + MID_TIME + b')',
     b'TM=VAL(MID$(TIME$,8,1))'),
    (20, PRINT + FF_VAL + b'(' + FF_MID + b'("18:52:37",' + dig(8) + b','
     + dig(1) + b'))', b'PRINT VAL(MID$("18:52:37",8,1))'),
    (30, PRINT + FF_LEN + b'(' + MID_TIME + b')',
     b'PRINT LEN(MID$(TIME$,8,1))'),
]
FUNC_REPORT = ['labels: {}']        # no "unknown FF token" lines
FUNC_ROWS = [' 7', ' 1']

LAB_LINES = [
    # 5 STOP ON:ON STOP GOSUB *TYPE   (dropped -> REM)
    (5, b'\x90 \x95:\x95 \x90 \x8d \xf5TYPE', b'REM'),
    (10, b'\x8d\xf5TYPE', b'GOSUB *TYPE'),
    (20, b'\x8bA\xf1\x12 \xdd\xf5OK', b'IF A=1 THEN *OK'),
    (30, b'\x91"bad":\x81', b'PRINT "bad":END'),
    (35, b'\x89\xf5NOPE', b'GOTO *NOPE'),       # never executed
    (40, b'\xf5TYPE:A\xf1\x12:\x8e', b'*TYPE:A=1:RETURN'),
    (50, b'\xf5OK', b'*OK'),
    (60, b'\x91"labrun";A:\x81', b'PRINT "labrun";A:END'),
]
LAB_REPORT = [
    "labels: {'TYPE': 40, 'OK': 50}",
    'line 5: dropped STOP ON',
    'line 5: dropped ON STOP GOSUB',
    'line 35: UNDEFINED label *NOPE (passed through)',
]

# Label rules of the interpreter (src/token.c label scan, src/exec.c
# find_label). Runs: 10 -> *ABC (30, prints X) -> 40 GOSUB -> 70 -> 50
# -> *FIN (80) -> 90 prints "lab2 2". Lines 100+ are never executed.
LAB2_LINES = [
    (10, b'\x89\xf5abc', b'GOTO *abc'),                 # case folds
    (20, b'\x91"bad":\x81', b'PRINT "bad":END'),
    # definition run straight into a PRINT token: needs the ':'
    (30, b'\xf5ABC\x91"X"', b'*ABC:PRINT "X"'),
    (40, b'\x8d\xf5Sub.1', b'GOSUB *Sub.1'),            # dotted, case folds
    # reference run straight into an ELSE token: needs the space
    (50, b'\x8bA\xf1\x13 \xdd\xf5fin\x9f\xf5ABC', b'IF A=2 THEN *fin ELSE *ABC'),
    (60, b'\x91"bad2":\x81', b'PRINT "bad2":END'),
    # leading spaces before the definition
    (70, b'  \xf5SUB.1:A\xf1\x13:\x8e', b'*SUB.1:A=2:RETURN'),
    (80, b'\xf5FIN', b'*FIN'),
    (90, b'\x91"lab2";A:\x81', b'PRINT "lab2";A:END'),
    (100, b'\x89\xf5SUB', b'GOTO *SUB'),        # *SUB.1 is not *SUB
    (110, b'\x89\xf5' + b'100', b'GOTO *100'),  # not a label: syntax error
    (120, b'\xf5' + b'100', b'*100'),           # nor a definition
    (130, b'\xf5ABC', b'*ABC'),                 # duplicate: first one wins
]
LAB2_REPORT = [
    "labels: {'ABC': 30, 'SUB.1': 70, 'FIN': 80}",
    'line 100: UNDEFINED label *SUB (passed through)',
    'line 110: UNDEFINED label *100 (passed through)',
]
LAB2_ROWS = ['X', 'lab2 2']


def expected_text(lines):
    return b''.join(b'%d %s\r\n' % (ln[0], ln[2]) for ln in lines)


def check_program(name, lines, cbin, outdir, want_report=None, tail=PADDED):
    """Convert with Python and C, compare both with the expected text
    (and report). Returns the Python converter's output."""
    data = program([(ln[0], ln[1]) for ln in lines], tail)
    want = expected_text(lines)
    log = []
    got, _ = p88_to_bas.convert(data, log.append)
    chk(f'{name}: python text', got, want)
    if want_report is not None:
        chk(f'{name}: python report', log, want_report)
    tokfile = os.path.join(outdir, name + '.tok')
    with open(tokfile, 'wb') as f:
        f.write(data)
    r = subprocess.run([cbin, tokfile], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    chk(f'{name}: C exit status', r.returncode, 0)
    chk(f'{name}: C text', r.stdout, want)
    chk(f'{name}: C text == python text', r.stdout, got)
    if want_report is not None:
        crep = r.stderr.decode('latin-1').splitlines()
        # the C report ends with its own "converted: ..." summary line;
        # drop only that line and compare the whole remaining report, so
        # an unexpected warning anywhere in it fails the check
        if crep and crep[-1].startswith('converted:'):
            crep.pop()
        chk(f'{name}: C report', crep, want_report)
    return got


def run_host(host, outdir, name, text):
    """Write <name>.BAS, run it in the host interpreter and return the
    text rows of its final screen (trailing blank rows dropped), or None
    when the run failed."""
    global fails
    with open(os.path.join(outdir, name + '.BAS'), 'wb') as f:
        f.write(text)
    env = dict(os.environ)
    env.pop('P8P_DISK', None)
    try:
        r = subprocess.run([os.path.abspath(host), name + '.BAS'], cwd=outdir,
                           env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=60)
    except subprocess.TimeoutExpired:
        print(f'FAIL {name}: host interpreter timed out')
        fails += 1
        return None
    chk(f'{name}: host exit status', r.returncode, 0)
    rows = []
    for line in r.stdout.decode('latin-1').splitlines():
        m = re.match(r'(\d\d)\|(.*)$', line)
        if m:
            # Row 24 is the function-key strip (screen.c FKEY_DEFAULTS),
            # not program output; the suite's Tier B files keep it, this
            # test's fixtures do not model it.
            if m.group(1) == '24' and m.group(2).startswith('list'):
                continue
            rows.append(m.group(2))
    while rows and rows[-1] == '':
        rows.pop()
    return rows


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        return 2
    cbin, outdir = sys.argv[1], sys.argv[2]
    host = sys.argv[3] if len(sys.argv) == 4 else None
    os.makedirs(outdir, exist_ok=True)

    # the payload helpers against the hand-written patterns
    chk('mbf_s 1.0', mbf_s(1.0), S_1_0)
    chk('mbf_s -2.5', mbf_s(-2.5), S_M2_5)
    chk('mbf_s 2^-24', mbf_s(2.0 ** -24), S_2M24)
    chk('mbf_d 1.0', mbf_d(1.0), D_1_0)
    chk('mbf_d -2.5', mbf_d(-2.5), D_M2_5)
    chk('mbf_d 1+2^-30', mbf_d(1 + 2.0 ** -30), D_1P30)

    # the decoders themselves
    chk('single 1.0', p88_detok.mbf_single(S_1_0), '1.0')
    chk('single 0.5', p88_detok.mbf_single(S_0_5), '0.5')
    chk('single -2.5', p88_detok.mbf_single(S_M2_5), '(-2.5)')
    chk('single 0.1', p88_detok.mbf_single(S_0_1), T_S_0_1)
    chk('single 2^24', p88_detok.mbf_single(S_2P24), T_S_2P24)
    chk('single 2^-24', p88_detok.mbf_single(S_2M24), T_S_2M24)
    chk('single max', p88_detok.mbf_single(S_MAX), T_S_MAX)
    chk('single max/2', p88_detok.mbf_single(S_HALFMAX), T_S_HALFMAX)
    chk('single zero', p88_detok.mbf_single(S_ZERO), '0!')
    chk('double 1.0', p88_detok.mbf_double(D_1_0), '1.0#')
    chk('double 0.5', p88_detok.mbf_double(D_0_5), '0.5#')
    chk('double -2.5', p88_detok.mbf_double(D_M2_5), '(-2.5#)')
    chk('double 2^24', p88_detok.mbf_double(D_2P24), T_D_2P24)
    chk('double 1e-05', p88_detok.mbf_double(D_1EM5), T_D_1EM5)
    chk('double 1+2^-30', p88_detok.mbf_double(D_1P30), T_D_1P30)
    chk('double 1+2^-55', p88_detok.mbf_double(D_1P55), '1.0#')
    chk('double zero', p88_detok.mbf_double(D_ZERO), '0#')

    # the detokenizer (listing tool) prints the same constants
    for no, body, text in NUM_LINES:
        chk(f'detok line {no}', p88_detok.detok(body), text.decode('latin-1'))

    # both converters, whole files; the three file endings frame alike
    for suffix, tail in (('', PADDED), ('_M', MARKER), ('_C', CUT)):
        check_program('CONVNUM' + suffix, NUM_LINES, cbin, outdir, tail=tail)
        check_program('CONVEND' + suffix, END_LINES, cbin, outdir, tail=tail)
    evaltext = check_program('CONVEVAL', EVAL_LINES, cbin, outdir)
    for no, body, text in FUNC_LINES:
        chk(f'detok FUNC line {no}', p88_detok.detok(body),
            text.decode('latin-1'))
    functext = check_program('CONVFUNC', FUNC_LINES, cbin, outdir, FUNC_REPORT)
    text = check_program('CONVLAB', LAB_LINES, cbin, outdir, LAB_REPORT)
    with open(os.path.join(outdir, 'CONVLAB.BAS'), 'wb') as f:
        f.write(text)
    lab2text = check_program('CONVLAB2', LAB2_LINES, cbin, outdir, LAB2_REPORT)

    # what the host interpreter makes of the converted text
    if host:
        rows = run_host(host, outdir, 'CONVEVAL', evaltext)
        if rows is not None:
            want = [ln[3] for ln in EVAL_LINES]
            for k, ln in enumerate(EVAL_LINES):
                got = rows[k] if k < len(rows) else None
                chk(f'CONVEVAL line {ln[0]} ({ln[2].decode()}) prints',
                    got, ln[3])
            chk('CONVEVAL: screen rows', rows, want)
        rows = run_host(host, outdir, 'CONVFUNC', functext)
        if rows is not None:
            chk('CONVFUNC: screen rows', rows, FUNC_ROWS)
        rows = run_host(host, outdir, 'CONVLAB2', lab2text)
        if rows is not None:
            chk('CONVLAB2: screen rows', rows, LAB2_ROWS)
    else:
        print('NOTE: no host interpreter given -- evaluation checks skipped')

    if fails:
        print(f'{fails} check(s) failed')
        return 1
    print('p88conv_test ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
