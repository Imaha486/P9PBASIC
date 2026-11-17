#!/usr/bin/env python3
"""Detokenize PC-88 N88-BASIC (Disk BASIC) tokenized programs (MENU.ALP / menu
from MA2026A.D88) into readable text. Work-in-progress RE tool, 2026-11-17.

Format (reverse-engineered from the actual files):
  line = [link u16 LE = file offset of next line][lineno u16 LE][body][0x00]
  The link sometimes drifts by 1 from the true next offset, so the parser
  searches link-2..link+2 for a plausible next header.
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
FUNC = {
 0x85:'INT?',0x90:'INP',0x92:'LEN',0x94:'VAL',0x97:'PEEK',
 0x82:'RIGHT$?',0x93:'STR$',0x9A:'HEX$',
}
OPS = {0xF0:'>',0xF1:'=',0xF2:'<',0xF3:'+',0xF4:'-',0xF5:'*',0xF6:'/',0xF7:'^',
       0xF8:'AND',0xF9:'OR',0xFA:'XOR',0xFB:'EQV',0xFC:'IMP',0xFD:'MOD',0xFE:'\\'}

def mbf_single(b):
    if len(b) < 4: return '?'
    exp = b[3]
    if exp == 0: return '0'
    bits = (b[0] | (b[1] << 8) | (b[2] << 16) | (0x80 << 24))
    return repr(bits * (2.0 ** (exp - 127 - 24)))

def parse_lines(d):
    lines = []
    p = 0
    prev_no = -1
    while p + 4 <= len(d):
        link, no = struct.unpack_from('<HH', d, p)
        if link == 0 or no == 0: break
        # find next line start near link
        nxt = None
        for cand in (link, link-1, link+1, link-2, link+2):
            if cand <= p + 4 or cand + 4 > len(d): continue
            l2, n2 = struct.unpack_from('<HH', d, cand)
            if no < n2 < no + 2000 and l2 > cand:
                nxt = cand; break
        if nxt is None:
            # last line: body runs to first 00 after p+4 (best effort)
            end = d.find(b'\x00', p+4)
            nxt = (end + 1) if end >= 0 else len(d)
            lines.append((no, d[p+4:nxt]))
            break
        body = d[p+4:nxt]
        if body.endswith(b'\x00'): body = body[:-1]
        lines.append((no, body))
        prev_no = no
        p = nxt
    return lines

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
            v = struct.unpack_from('<h', body, i+1)[0]; out.append(str(v)); i += 3; continue
        if b == 0x1D:
            out.append(mbf_single(body[i+1:i+5])); i += 5; continue
        if b == 0x1F:
            out.append('<DBL>'); i += 9; continue
        if b == 0xFF:
            t = body[i+1]
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
    path = sys.argv[1]
    d = open(path, 'rb').read()
    for no, body in parse_lines(d):
        print(f'{no} {detok(body)}')

if __name__ == '__main__':
    main()
