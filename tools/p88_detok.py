#!/usr/bin/env python3
"""Detokenize PC-88 N88-BASIC (Disk BASIC) tokenized programs (MENU.ALP / menu
from MA2026A.D88) into readable text. Work-in-progress RE tool, 2026-11-17.

Format (reverse-engineered from the actual files):
  line = [link u16 LE = file offset of next line][lineno u16 LE][body][0x00]
  Lines are framed by walking the body token by token (body_end): the
  line ends at the first 00 met at a token boundary, and the next header
  (or the 00 00 end marker / end of file) starts right after it. The
  stored link drifts by 1 in the real files and is not used to frame
  anything; only a zero link (the end marker) is read. A chain that does
  not hold together is rejected with ValueError.
  body = MS-BASIC-style tokens (GW-BASIC order, PC-88 extensions in C0-FE),
         plain-ASCII variable names, string literals raw (Shift_JIS).
  numbers: 0B=&O+u16 0C=&H+u16 0E=line-ref u16 0F=int8 11-1A=digits 0-9
           1C=int16LE 1D=MBF single 1F=MBF double
  8F E9  = apostrophe comment: rest of line is raw text.
  84 DATA: contents raw until ':' or end of line.
"""
import struct, sys

TOK = {
 0x81:'END',0x82:'FOR',0x83:'NEXT',0x84:'DATA',0x85:'INPUT',0x86:'DIM',0x87:'READ',
 0x88:'LET',0x89:'GOTO',0x8A:'RUN',0x8B:'IF',0x8C:'RESTORE',0x8D:'GOSUB',0x8E:'RETURN',
 0x8F:"'",0x90:'STOP',0x91:'PRINT',0x92:'CLEAR',0x93:'LIST',0x94:'NEW',0x95:'ON',
 0x96:'WAIT',0x97:'DEF',0x98:'POKE',0x99:'CONT',0x9A:'OUT',0x9B:'LPRINT',0x9C:'LLIST',
 0x9D:'CONSOLE',0x9E:'WIDTH',0x9F:'ELSE',0xA0:'TRON',0xA1:'TROFF',0xA2:'SWAP',
 0xA3:'ERASE',0xA4:'ERROR',0xA5:'RESUME',0xA6:'DELETE',0xA7:'AUTO',0xA8:'RENUM',
 0xA9:'DEFSTR',0xAA:'DEFINT',0xAB:'DEFSNG',0xAC:'DEFDBL',
 0xAE:'LINE',0xAF:'WHILE',0xB0:'WEND',
 # PC-88 extensions identified from context (MENU.ALP / menu, 2026-11-17)
 0xCB:'COLOR',0xCE:'CLS',0xD3:'SCREEN',0xD4:'BLOAD',0xD6:'LOCATE',
 0xDC:'TO',0xDD:'THEN',
}
# FF xx function tokens. FF83 = MID$ and FFDC = TIME$ are confirmed by
# FC88V42 MENU.ALP: line 286 is TM=VAL(<FF83>(<FFDC>,8,1)) and line 460
# keeps the plain-text twin 'PM=VAL(MID$(TIME$,1,2)). With MID$ at 83 the
# two-argument string function at 82 is RIGHT$ (LEFT$/RIGHT$/MID$ block,
# and the use is right-justifying: RIGHT$("  "+STR$(I),3)). FF85 is still
# a guess from context (the converters commit to INT).
FUNC = {
 0x85:'INT?',0x90:'INP',0x92:'LEN',0x94:'VAL',0x97:'PEEK',
 0x82:'RIGHT$',0x83:'MID$',0x93:'STR$',0x9A:'HEX$',
 0xDC:'TIME$',
}
OPS = {0xF0:'>',0xF1:'=',0xF2:'<',0xF3:'+',0xF4:'-',0xF5:'*',0xF6:'/',0xF7:'^',
       0xF8:'AND',0xF9:'OR',0xFA:'XOR',0xFB:'EQV',0xFC:'IMP',0xFD:'MOD',0xFE:'\\'}

# MBF numbers are sign * 0.1fff...(binary) * 2^(exp-128): the mantissa is a
# fraction in [0.5, 1) whose leading 1 is implied (its bit position holds the
# sign instead), and exp == 0 means zero. 1.0 is exponent 0x81 with an
# all-zero stored mantissa. src/p88conv.c mirrors both functions.

# Spelling of numeric constants. The text is read back by the interpreter,
# whose parser (src/num.c str_to_num) takes the type from the spelling:
#   suffix % / ! / #        -> integer / single / double
#   exponent letter D       -> double
#   exponent letter E       -> single
#   no '.', value <= 32767  -> integer
#   more than 7 digits      -> double   (digits counted without leading zeros)
#   otherwise               -> single
# and a single literal above 1.70141E+38 is an Overflow error. The digits
# are Python's repr() of the value (shortest string that reads back to the
# same binary64), so the value read back is exact; the letter / suffix is
# chosen so the type read back is the token's type:
#   single: plain form as is when it has <= 7 digits, else with '!';
#           exponent form with 'E'; above the literal limit the exact
#           single product (half*2), which the interpreter's single
#           multiply does not range-check.
#   double: plain form as is when it has > 7 digits, else with '#';
#           exponent form with 'D'.
# Negative constants are parenthesized: the interpreter's unary minus binds
# looser than '^', so a bare "-2.5^2" would read as -(2.5^2).
# src/p88conv.c (put_num_literal / put_int16) mirrors this byte for byte.

SNG_LITERAL_MAX = 1.70141e38    # src/num.c: larger single literals overflow

def _typed(a, dbl):
    """Spelling of the positive float a that reads back as a single
    (dbl false) or a double (dbl true) with exactly this value."""
    r = repr(a)
    if 'e' in r:
        return r.replace('e', 'D' if dbl else 'E')
    nd = len(r.replace('.', '').lstrip('0'))    # the parser's digit count
    if dbl:
        return r if nd > 7 else r + '#'
    return r if nd <= 7 else r + '!'

def num_literal(v, dbl):
    """Typed literal text for the nonzero finite float v."""
    a = abs(v)
    if not dbl and a > SNG_LITERAL_MAX:
        s = '(' + _typed(a / 2, False) + '*2)'
    else:
        s = _typed(a, dbl)
    return '(-' + s + ')' if v < 0 else s

def int_literal(v):
    """Text of a signed 16-bit integer constant. -32768 has no integer
    literal (32768 reads as a single), so it is spelled as integer
    arithmetic that stays integer."""
    if v >= 0: return str(v)
    if v == -32768: return '(-32767-1)'
    return f'({v})'

def mbf_single(b):
    """[m0][m1][m2: sign|m2][exp], 24-bit mantissa, hidden bit at bit 23:
    value = sign * bits * 2^(exp-128-24) = sign * (1 + frac/2^23) * 2^(exp-129).
    A payload shorter than 4 bytes is a truncated token: ValueError."""
    if len(b) < 4: raise ValueError('truncated MBF single payload')
    exp = b[3]
    if exp == 0: return '0!'
    sign = -1.0 if (b[2] & 0x80) else 1.0
    bits = b[0] | (b[1] << 8) | ((b[2] & 0x7F) << 16) | (1 << 23)
    return num_literal(sign * bits * (2.0 ** (exp - 128 - 24)), False)

def mbf_double(b):
    """[m0..m5][m6: sign|m6][exp], 56-bit mantissa, hidden bit at bit 55:
    value = sign * bits * 2^(exp-128-56). The 56-bit mantissa is rounded
    (half to even) to the 53 bits of a binary64, the interpreter's own
    double type; see docs/TOKEN-FORMAT.md. A payload shorter than 8 bytes
    is a truncated token: ValueError."""
    if len(b) < 8: raise ValueError('truncated MBF double payload')
    exp = b[7]
    if exp == 0: return '0#'
    sign = -1.0 if (b[6] & 0x80) else 1.0
    frac = (b[6] & 0x7F) << 48 | b[5] << 40 | b[4] << 32 | b[3] << 24 \
        | b[2] << 16 | b[1] << 8 | b[0]
    bits = frac | (1 << 55)
    return num_literal(sign * bits * (2.0 ** (exp - 128 - 56)), True)

MAX_LINENO = 65529              # largest line number N88-BASIC accepts

# Line framing. src/p88conv.c (token_width / body_end / parse_lines)
# mirrors these three functions decision for decision: both sides must
# frame, and reject, the same inputs.
#
# A line is framed by its own bytes, not by its link field: body_end walks
# the body token by token and the line ends at the first 00 it meets at a
# token boundary. The next header starts at that terminator + 1 and
# nowhere else. The link field is not consulted (the real files have it
# 1 off, the last line of FC88 MENU.ALP 3 off); whatever it says, exact,
# one low, one high or garbage, the walk decides.

NO_TERM = -1                    # body_end: the buffer ends before a 00
CUT_TOKEN = -2                  # body_end: the buffer ends inside a token

def token_width(b):
    """Bytes the token starting with b occupies: the byte itself plus its
    payload. 0B/0C/0E/1C carry 2 payload bytes, 0F 1, 1D 4 (MBF single),
    1F 8 (MBF double), FF its function / extension code; everything else
    (keywords, operators, digits, plain text, bytes the tables do not
    know) is one byte. detok and p88_to_bas.convert_body step by the same
    widths."""
    if b in (0x0B, 0x0C, 0x0E, 0x1C): return 3
    if b == 0x0F: return 2
    if b == 0x1D: return 5
    if b == 0x1F: return 9
    if b == 0xFF: return 2
    return 1

def body_end(d, off):
    """Offset of the 00 that terminates the line whose body starts at off:
    the first 00 at a token boundary. The body is walked the way detok and
    convert_body read it, so a 00 inside a token (a number payload, the
    second byte of FF xx) is content and is stepped over with its token.
    Strings, 8F E9 comments and DATA contents are raw text and end at the
    first 00. Returns NO_TERM when the buffer ends before a terminator and
    CUT_TOKEN when it ends inside a token's payload."""
    i, n = off, len(d)
    data = quote = False
    while i < n:
        b = d[i]
        if b == 0:
            return i
        if data:                        # DATA: raw until an unquoted ':'
            if b == 0x22: quote = not quote
            elif b == 0x3A and not quote: data = False
            i += 1; continue
        if b == 0x22:                   # string literal
            i += 1
            while i < n and d[i] != 0x22 and d[i] != 0: i += 1
            if i < n and d[i] == 0x22: i += 1
            continue
        if b == 0x8F and i + 1 < n and d[i+1] == 0xE9:
            z = d.find(b'\x00', i + 2)          # comment: raw to the 00
            return z if z >= 0 else NO_TERM
        if b == 0xFF and i + 1 < n and d[i+1] == 0:
            i += 1; continue            # FF 00 is no token: 00 terminates
        w = token_width(b)
        if i + w > n:
            return CUT_TOKEN
        if b == 0x84: data = True
        i += w
    return NO_TERM

def parse_lines(d):
    """[(line number, body bytes)] of a tokenized program. Raises
    ValueError when d is not one. Every line, the last one included, is
    framed the same way: its body runs from the header to the terminator
    body_end finds, and what follows the terminator must be either the
    end of the program (the 00 00 marker, or the file ending there) or
    the next header, whose line number must be larger than this one's
    and within 1..MAX_LINENO. Rejected: a truncated header, a line
    number out of range or not increasing, a line with no terminator or
    with a token cut off by the end of the file."""
    lines = []
    n = len(d)
    if n < 4:
        if any(d):
            raise ValueError('not a tokenized program: truncated line '
                             'header at offset 0x0')
        return lines
    if d[0] == 0 and d[1] == 0:         # end marker: empty program
        return lines
    p = prev = 0
    while True:                         # a whole header is at p
        no = d[p+2] | d[p+3] << 8
        if no == 0 or no > MAX_LINENO:
            raise ValueError('not a tokenized program: implausible line '
                             f'number {no} at offset 0x{p:X}')
        if no <= prev:
            raise ValueError(f'line chain broken at offset 0x{p:X}: line '
                             f'number {no} does not follow line {prev}')
        term = body_end(d, p + 4)
        if term == CUT_TOKEN:
            raise ValueError(f'line {no} at offset 0x{p:X}: token cut off '
                             'by the end of the file')
        if term < 0:
            raise ValueError(f'line {no} at offset 0x{p:X}: no 00 '
                             'terminator before the end of the file')
        lines.append((no, d[p+4:term]))
        q = term + 1
        if not any(d[q:q+2]):           # 00 00 marker, or the file ends
            break
        if q + 4 > n:
            raise ValueError(f'line chain broken after line {no}: truncated '
                             f'line header at offset 0x{q:X}')
        prev = no
        p = q
    return lines

def need(body, i, k):
    """Raise ValueError unless the k payload bytes after body[i] exist
    (src/p88conv.c convert_body returns -1 in the same places)."""
    if i + k >= len(body):
        raise ValueError(f'truncated token {body[i]:02X} at body offset {i}')

def detok(body):
    out = []
    i = 0
    raw = False      # after ' (8F E9) : raw to EOL
    data = False     # after DATA: raw until ':'
    in_str = False
    while i < len(body):
        b = body[i]
        if raw:
            out.append(body[i:].decode('shift_jis', 'replace')); break
        if in_str:
            if b == 0x22: in_str = False; out.append('"')
            else: out.append(bytes([b]).decode('shift_jis', 'replace'))
            i += 1; continue
        if data and b != 0x3A:
            out.append(bytes([b]).decode('shift_jis', 'replace')); i += 1; continue
        if data and b == 0x3A:
            data = False
        if b == 0x22:
            in_str = True; out.append('"'); i += 1; continue
        if b == 0x8F and i + 1 < len(body) and body[i+1] == 0xE9:
            out.append("'"); raw = True; i += 2; continue
        if b in (0x0B, 0x0C, 0x0E, 0x1C): need(body, i, 2)
        if b == 0x0F: need(body, i, 1)
        if b == 0x1D: need(body, i, 4)
        if b == 0x1F: need(body, i, 8)
        if b == 0x0B:
            v = struct.unpack_from('<H', body, i+1)[0]; out.append(f'&O{v:o}'); i += 3; continue
        if b == 0x0C:
            v = struct.unpack_from('<H', body, i+1)[0]; out.append(f'&H{v:X}'); i += 3; continue
        if b == 0x0E:
            v = struct.unpack_from('<H', body, i+1)[0]; out.append(str(v)); i += 3; continue
        if b == 0x0F:
            out.append(str(body[i+1])); i += 2; continue
        if 0x11 <= b <= 0x1A:
            out.append(str(b - 0x11)); i += 1; continue
        if b == 0x1C:
            v = struct.unpack_from('<h', body, i+1)[0]; out.append(int_literal(v)); i += 3; continue
        if b == 0x1D:
            out.append(mbf_single(body[i+1:i+5])); i += 5; continue
        if b == 0x1F:
            out.append(mbf_double(body[i+1:i+9])); i += 9; continue
        if b == 0xFF:
            t = body[i+1] if i + 1 < len(body) else 0
            if t in FUNC: out.append(FUNC[t])
            elif t in (0xDD, 0xDE, 0xDF): out.append(f'<FF{t:02X}>')
            else: out.append(f'<FF{t:02X}>')
            i += 2; continue
        if b in OPS:
            out.append(f' {OPS[b]} ' if OPS[b].isalpha() else OPS[b]); i += 1; continue
        if b in TOK:
            w = TOK[b]
            out.append(w)
            if b == 0x84: data = True
            if b not in (0x84,): out.append(' ')
            i += 1; continue
        if 0x80 <= b <= 0xFE:
            out.append(f'<{b:02X}>'); i += 1; continue
        out.append(chr(b) if 0x20 <= b < 0x7F else bytes([b]).decode('shift_jis', 'replace'))
        i += 1
    return ''.join(out)

def main():
    if len(sys.argv) != 2:
        print(f'usage: {sys.argv[0]} <tokenized-file>', file=sys.stderr)
        return 1
    path = sys.argv[1]
    try:
        d = open(path, 'rb').read()
        text = [f'{no} {detok(body)}' for no, body in parse_lines(d)]
    except (OSError, ValueError) as e:
        print(f'p88_detok.py: {path}: {e}', file=sys.stderr)
        return 1
    for line in text:
        print(line)
    return 0

if __name__ == '__main__':
    sys.exit(main())
