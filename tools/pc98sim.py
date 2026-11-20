#!/usr/bin/env python3
"""Minimal PC-98 + MS-DOS test bench for P9PBASIC.EXE (unicorn based).

Emulates: 8086 CPU, 1MB RAM, text VRAM, keyboard 8251 (port 41h/43h, IRQ1),
VSYNC IRQ2, master PIC mask/EOI, text GDC status + CSRW/CSRFORM tracking,
graphics VRAM planes, and a small subset of MS-DOS INT 21h.

usage: pc98sim.py EXE [args] --keys "script" [--dump] [--gfx out.png] [--steps N]
"""
import sys, os, struct, time, argparse
from unicorn import *
from unicorn.x86_const import *

ap = argparse.ArgumentParser()
ap.add_argument('exe')
ap.add_argument('args', nargs='*')
ap.add_argument('--keys', default='')
ap.add_argument('--gfx', default=None)
ap.add_argument('--chunks', type=int, default=4000)
ap.add_argument('--trace-int', action='store_true')
ap.add_argument('--cwd', default='.')
ap.add_argument('--idle', type=int, default=300)
ap.add_argument('--ime', action='append', default=[], help='text put into the BIOS key buffer at each \\I')
ap.add_argument('--attr', default=None)
opt = ap.parse_args()

mu = Uc(UC_ARCH_X86, UC_MODE_16)
mu.mem_map(0, 0x100000)

PSP = 0x0800
log = []

# ---------------- load MZ ----------------
data = open(opt.exe, 'rb').read()
(sig, lastp, pages, nreloc, hdrpar, minalloc, maxalloc, ss, sp, csum, ip, cs, reloff) = struct.unpack('<2s12H', data[:26])
assert sig == b'MZ'
imgsize = pages * 512 - (512 - lastp if lastp else 0) - hdrpar * 16
img = data[hdrpar * 16: hdrpar * 16 + imgsize]
loadseg = PSP + 0x10
mu.mem_write(loadseg * 16, img)
for i in range(nreloc):
    o, s = struct.unpack('<HH', data[reloff + i * 4: reloff + i * 4 + 4])
    a = (loadseg + s) * 16 + o
    v = struct.unpack('<H', mu.mem_read(a, 2))[0]
    mu.mem_write(a, struct.pack('<H', (v + loadseg) & 0xFFFF))

# PSP
psp = bytearray(256)
psp[0:2] = b'\xCD\x20'
ENVSEG = 0x0700
endseg = 0x9FFF
psp[2:4] = struct.pack('<H', endseg)
psp[0x2C:0x2E] = struct.pack('<H', ENVSEG)
tail = (' ' + ' '.join(opt.args)) if opt.args else ''
psp[0x80] = len(tail)
psp[0x81:0x81 + len(tail)] = tail.encode()
psp[0x81 + len(tail)] = 0x0D
mu.mem_write(PSP * 16, bytes(psp))
env = b'PATH=A:\\\x00COMSPEC=A:\\COMMAND.COM\x00\x00\x01\x00A:\\P9PBASIC.EXE\x00'
mu.mem_write(ENVSEG * 16, env)

# memory blocks: simple list of (seg, paras, owner)
prog_end = loadseg + (len(img) + 15) // 16 + minalloc
blocks = [[PSP, endseg - PSP, True]]   # PSP block initially owns everything

mu.reg_write(UC_X86_REG_CS, (loadseg + cs) & 0xFFFF)
mu.reg_write(UC_X86_REG_IP, ip)
mu.reg_write(UC_X86_REG_SS, (loadseg + ss) & 0xFFFF)
mu.reg_write(UC_X86_REG_SP, sp)
mu.reg_write(UC_X86_REG_DS, PSP)
mu.reg_write(UC_X86_REG_ES, PSP)

# IVT: point all vectors to an IRET stub at F000:0000
mu.mem_write(0xF0000, b'\xCF')
for v in range(256):
    mu.mem_write(v * 4, struct.pack('<HH', 0x0000, 0xF000))

# ---------------- hardware state ----------------
pic_mask = 0xFF & ~0x01
kbd_queue = []        # pending scan codes
kbd_data = 0
kbd_ready = False
vsync_armed = False
beep = False
gdc_cmd = None
gdc_params = []
cursor = [0, False]   # address, visible
exited = [False, 0]
irq_pending = set()
port_log = {}
port_last = {}
plane_bank = 0

def inb(port):
    global kbd_ready
    if port == 0x41:
        kbd_ready = False
        return kbd_data
    if port == 0x43:
        return 0x85 | (0x02 if kbd_ready else 0)
    if port == 0x60:
        return 0x04 | (0x20 if (int(time.time() * 120) & 1) else 0)
    if port == 0xA0:
        return 0x04
    if port == 0x02:
        return pic_mask
    return 0xFF

def outb(port, val):
    global pic_mask, vsync_armed, beep, gdc_cmd, gdc_params
    port_log[port] = port_log.get(port, 0) + 1
    port_last[port] = val
    if port == 0x02: pic_mask = val
    elif port == 0x64: vsync_armed = True
    elif port == 0x37:
        if val == 6: beep = True
        elif val == 7: beep = False
    elif port == 0x62:
        gdc_cmd = val; gdc_params = []
    elif port == 0x60:
        gdc_params.append(val)
        if gdc_cmd == 0x49 and len(gdc_params) == 3:
            cursor[0] = gdc_params[0] | (gdc_params[1] << 8)
        if gdc_cmd == 0x4B and len(gdc_params) == 1:
            cursor[1] = bool(val & 0x80)

def hook_in(uc, port, size, user):
    return inb(port)

def hook_out(uc, port, size, value, user):
    outb(port, value & 0xFF)

mu.hook_add(UC_HOOK_INSN, hook_in, None, 1, 0, UC_X86_INS_IN)
mu.hook_add(UC_HOOK_INSN, hook_out, None, 1, 0, UC_X86_INS_OUT)

# ---------------- DOS ----------------
files = {}
next_handle = [5]
dta = [PSP * 16 + 0x80]
find_state = {}

def regs(*names):
    return [mu.reg_read(getattr(sys.modules['unicorn.x86_const'], 'UC_X86_REG_' + n)) for n in names]

def setr(name, v):
    mu.reg_write(getattr(sys.modules['unicorn.x86_const'], 'UC_X86_REG_' + name), v & 0xFFFF)

def set_cf(c):
    f = mu.reg_read(UC_X86_REG_EFLAGS)
    f = (f | 1) if c else (f & ~1)
    mu.reg_write(UC_X86_REG_EFLAGS, f)

def read_asciiz(seg, off):
    a = seg * 16 + off
    s = b''
    while True:
        c = mu.mem_read(a, 1)[0]
        if c == 0: break
        s += bytes([c]); a += 1
    return s.decode('latin1')

def hostpath(p):
    p = p.replace('\\', '/')
    if len(p) > 1 and p[1] == ':': p = p[2:]
    p = p.lstrip('/')
    # case-insensitive match
    d = opt.cwd
    for name in os.listdir(d):
        if name.upper() == p.upper():
            return os.path.join(d, name)
    return os.path.join(d, p.upper())

def dos_alloc(paras):
    # first fit after existing blocks
    blocks.sort()
    cur = PSP
    for b in blocks:
        if b[0] - cur >= paras + 1 and cur > PSP: pass
    top = max(b[0] + b[1] for b in blocks)
    if top + paras + 1 > endseg:
        return None, endseg - top - 1
    blocks.append([top + 1, paras, True])
    return top + 1, 0

def int21():
    ah = (mu.reg_read(UC_X86_REG_AX) >> 8) & 0xFF
    al = mu.reg_read(UC_X86_REG_AX) & 0xFF
    ax, bx, cx, dx, ds, es = regs('AX', 'BX', 'CX', 'DX', 'DS', 'ES')
    if opt.trace_int: log.append('INT21 AH=%02X' % ah)
    set_cf(False)
    if ah == 0x30: setr('AX', 0x1E03); setr('BX', 0); setr('CX', 0)
    elif ah == 0x35:
        o, s = struct.unpack('<HH', mu.mem_read(al * 4, 4)); setr('BX', o); setr('ES', s)
    elif ah == 0x25: mu.mem_write(al * 4, struct.pack('<HH', dx, ds))
    elif ah == 0x4A:
        for b in blocks:
            if b[0] == es:
                maxp = endseg - es
                others = [x for x in blocks if x[0] > es]
                if others: maxp = min(x[0] for x in others) - 1 - es
                if bx > maxp: setr('BX', maxp); setr('AX', 8); set_cf(True)
                else: b[1] = bx
                break
    elif ah == 0x48:
        seg, mx = dos_alloc(bx)
        if seg is None: setr('AX', 8); setr('BX', max(mx, 0)); set_cf(True)
        else: setr('AX', seg)
    elif ah == 0x49:
        for b in blocks:
            if b[0] == es: blocks.remove(b); break
    elif ah == 0x44:
        if al == 0:
            setr('DX', 0x80D3 if bx < 5 else 0x0002)
        else: setr('AX', 0)
    elif ah == 0x40:
        d = bytes(mu.mem_read(ds * 16 + dx, cx))
        if bx in (1, 2): log.append('DOSOUT:' + repr(d))
        elif bx in files:
            if cx == 0: files[bx].truncate()
            else: files[bx].write(d)
        setr('AX', cx)
    elif ah == 0x3F:
        if bx in files:
            d = files[bx].read(cx); mu.mem_write(ds * 16 + dx, d); setr('AX', len(d))
        else: setr('AX', 0)
    elif ah in (0x3D, 0x3C, 0x5B):
        name = read_asciiz(ds, dx); hp = hostpath(name)
        try:
            if ah == 0x3D:
                mode = {0: 'rb', 1: 'r+b', 2: 'r+b'}[al & 3]
                f = open(hp, mode)
            else: f = open(hp, 'w+b')
            h = next_handle[0]; next_handle[0] += 1; files[h] = f; setr('AX', h)
        except Exception:
            setr('AX', 2); set_cf(True)
    elif ah == 0x3E:
        if bx in files: files[bx].close(); del files[bx]
    elif ah == 0x42:
        if bx in files:
            off = (cx << 16) | dx
            if off & 0x80000000: off -= 1 << 32
            p = files[bx].seek(off, al); setr('AX', p & 0xFFFF); setr('DX', p >> 16)
        else: setr('AX', 0); setr('DX', 0)
    elif ah == 0x41:
        try: os.remove(hostpath(read_asciiz(ds, dx)))
        except Exception: setr('AX', 2); set_cf(True)
    elif ah == 0x56:
        try: os.rename(hostpath(read_asciiz(ds, dx)), hostpath(read_asciiz(es, mu.reg_read(UC_X86_REG_DI))))
        except Exception: setr('AX', 2); set_cf(True)
    elif ah == 0x2A: setr('CX', 2026); setr('DX', (11 << 8) | 16); setr('AX', (ah << 8) | 1)
    elif ah == 0x2C: t = time.localtime(); setr('CX', (t.tm_hour << 8) | t.tm_min); setr('DX', t.tm_sec << 8)
    elif ah in (0x2B, 0x2D): setr('AX', ah << 8)
    elif ah == 0x4C: exited[0] = True; exited[1] = al; mu.emu_stop()
    elif ah in (0x62, 0x51): setr('BX', PSP)
    elif ah == 0x19: setr('AX', (ah << 8) | 0)
    elif ah == 0x1A: dta[0] = ds * 16 + dx
    elif ah == 0x2F: setr('BX', (dta[0] & 0xF)); setr('ES', dta[0] >> 4)
    elif ah == 0x47: mu.mem_write(ds * 16 + mu.reg_read(UC_X86_REG_SI), b'\x00')
    elif ah == 0x09:
        a = ds * 16 + dx; s = b''
        while True:
            c = mu.mem_read(a, 1)[0]
            if c == ord('$') or len(s) > 500: break
            s += bytes([c]); a += 1
        log.append('DOSSTR:' + repr(s))
    elif ah in (0x4E, 0x4F):
        if ah == 0x4E:
            import fnmatch
            pat = read_asciiz(ds, dx).replace('\\', '/').split('/')[-1]
            if ':' in pat: pat = pat.split(':')[-1]
            pat = pat.upper().replace('*.*', '*')
            find_state['l'] = [n for n in sorted(os.listdir(opt.cwd)) if fnmatch.fnmatch(n.upper(), pat)]
        if not find_state.get('l'): setr('AX', 18); set_cf(True)
        else:
            n = find_state['l'].pop(0)
            rec = bytearray(43)
            rec[21] = 0x20
            sz = os.path.getsize(os.path.join(opt.cwd, n))
            rec[26:30] = struct.pack('<I', sz)
            nm = n.upper().encode()[:12]
            rec[30:30 + len(nm)] = nm
            mu.mem_write(dta[0], bytes(rec))
    elif ah == 0x36: setr('AX', 4); setr('BX', 1000); setr('CX', 512); setr('DX', 2000)
    elif ah == 0x33: setr('DX', 0)
    elif ah == 0x0B: setr('AX', 0)
    else:
        log.append('UNHANDLED INT21 AH=%02X' % ah)

def hook_intr(uc, intno, user):
    if intno == 0x21: int21()
    elif intno == 0x20: exited[0] = True; uc.emu_stop()
    else:
        # emulate by IVT dispatch
        log.append('INT %02X' % intno)

mu.hook_add(UC_HOOK_INTR, hook_intr)

# ---------------- key script -> scan codes ----------------
NORM = {0x1B: 0x00}
rows = ['1234567890-^\\', 'qwertyuiop@[', "asdfghjkl;:]", 'zxcvbnm,./_']
starts = [0x01, 0x10, 0x1D, 0x29]
SHIFTED = {}
shrows = ['!"#$%&\'()0=~|', 'QWERTYUIOP`{', 'ASDFGHJKL+*}', 'ZXCVBNM<>?_']
for r, st, sr in zip(rows, starts, shrows):
    for i, ch in enumerate(r):
        NORM[ord(ch)] = st + i
    for i, ch in enumerate(sr):
        if ord(ch) not in NORM or ch.isupper():
            SHIFTED[ord(ch)] = st + i
NORM[ord(' ')] = 0x34
NORM[13] = 0x1C; NORM[8] = 0x0E; NORM[9] = 0x0F
SPECIAL = {'r': 0x1C, 'n': 0x1C, 'e': 0x00, 'b': 0x0E, 's': 0x60, 'u': 0x3A, 'd': 0x3D, 'l': 0x3B,
           'R': 0x3C, 'i': 0x38, 'x': 0x39, 'h': 0x3E}

def script_to_events(s):
    ev = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]; i += 2
            if n in SPECIAL: ev += [SPECIAL[n], SPECIAL[n] | 0x80]
            elif n.isdigit():
                k = 0x62 + (int(n) - 1 if n != '0' else 9); ev += [k, k | 0x80]
            elif n == 'w': ev += ['wait']
            elif n == 'S': ev += ['shift']
            elif n == 'I': ev += ['ime']
            elif n == 'P': ev += [0x34]
            elif n == 'p': ev += [0xB4]
            continue
        o = ord(c)
        if o in NORM and not c.isupper():
            ev += [NORM[o], NORM[o] | 0x80]
        elif o in SHIFTED:
            ev += [0x70, SHIFTED[o], SHIFTED[o] | 0x80, 0xF0]
        i += 1
    return ev

events = script_to_events(opt.keys)

# ---------------- run loop ----------------
def inject(vec):
    cs, ip, sp, ss, fl = regs('CS', 'IP', 'SP', 'SS', 'EFLAGS')
    sp = (sp - 6) & 0xFFFF
    mu.mem_write(ss * 16 + sp, struct.pack('<HHH', ip, cs, fl & 0xFFFF))
    setr('SP', sp)
    mu.reg_write(UC_X86_REG_EFLAGS, fl & ~0x0200 & ~0x0100)
    o, s = struct.unpack('<HH', mu.mem_read(vec * 4, 4))
    setr('CS', s); setr('IP', o)

chunk = 0
wait_chunks = 0
vs_counter = 0
while not exited[0] and chunk < opt.chunks:
    cs, ip = regs('CS', 'IP')
    try:
        mu.emu_start(cs * 16 + ip, 0, count=20000)
    except UcError as e:
        cs, ip = regs('CS', 'IP')
        print('CPU error', e, 'at %04X:%04X' % (cs, ip))
        break
    chunk += 1
    fl = mu.reg_read(UC_X86_REG_EFLAGS)
    if not (fl & 0x200):
        continue
    # VSYNC
    vs_counter += 1
    if vsync_armed and not (pic_mask & 0x04) and vs_counter % 3 == 0:
        vsync_armed = False
        inject(0x0A)
        continue
    # keyboard
    if wait_chunks > 0:
        wait_chunks -= 1
        continue
    if events and not kbd_ready and not (pic_mask & 0x02):
        e = events.pop(0)
        if e == 'wait': wait_chunks = 30; continue
        if e == 'ime':
            data = opt.ime.pop(0).encode('shift_jis') if opt.ime else b''
            for b in data:
                head, tail, cnt = struct.unpack('<HHB', mu.mem_read(0x524, 5))
                if cnt >= 16: break
                mu.mem_write(tail, bytes([b, 0]))
                tail += 2
                if tail >= 0x522: tail = 0x502
                mu.mem_write(0x526, struct.pack('<H', tail)); mu.mem_write(0x528, bytes([cnt + 1]))
            wait_chunks = 5
            continue
        if e == 'shift': continue
        if e == 0xF0: e = 0xF0   # shift release
        kbd_data = e
        kbd_ready = True
        inject(0x09)
        wait_chunks = 2
        continue
    if not events and not kbd_ready and wait_chunks == 0:
        # idle: allow some more time then stop
        if chunk > opt.chunks - 1: break
        wait_chunks = 0
        if not hasattr(sys, '_idle'): sys._idle = 0
        sys._idle += 1
        if sys._idle > opt.idle: break

# ---------------- dump ----------------
def jis_ok(lo, hi):
    return True

print('----- TEXT VRAM ----- (exited=%s code=%d, cursor=%d vis=%s beep=%s chunks=%d)' % (exited[0], exited[1], cursor[0], cursor[1], beep, chunk))
tv = mu.mem_read(0xA0000, 4000)
ta = mu.mem_read(0xA2000, 4000)
for y in range(25):
    line = ''
    for x in range(80):
        lo = tv[(y * 80 + x) * 2]; hi = tv[(y * 80 + x) * 2 + 1]
        if hi:
            if not (lo & 0x80):
                c1 = (lo & 0x7F) + 0x20; c2 = hi & 0x7F
                s1 = ((c1 + 1) >> 1) + (0x70 if c1 <= 0x5E else 0xB0)
                s2 = c2 + ((0x20 if c2 >= 0x60 else 0x1F) if (c1 & 1) else 0x7E)
                try: line += bytes([s1, s2]).decode('shift_jis')
                except Exception: line += '#'
        else:
            line += chr(lo) if 0x20 <= lo < 0x7F else (' ' if lo == 0 else '?')
    print('%02d|%s' % (y, line.rstrip()))
if opt.attr:
    for y in [int(v) for v in opt.attr.split(',')]:
        print('A%02d|' % y + ' '.join('%02X' % ta[(y * 80 + x) * 2] for x in range(0, 40)))
for l in log[-15:]:
    print('LOG', l)
if opt.gfx:
    from PIL import Image
    im = Image.new('RGB', (640, 400))
    planes = [mu.mem_read(a, 32000) for a in (0xA8000, 0xB0000, 0xB8000, 0xE0000)]
    pal = [(0,0,0),(0,0,255),(255,0,0),(255,0,255),(0,255,0),(0,255,255),(255,255,0),(255,255,255)] + [(128,128,128)] + [(0,0,160),(160,0,0),(160,0,160),(0,160,0),(0,160,160),(160,160,0),(160,160,160)]
    px = im.load()
    for y in range(400):
        for xb in range(80):
            o = y * 80 + xb
            b0, b1, b2, b3 = planes[0][o], planes[1][o], planes[2][o], planes[3][o]
            for bit in range(8):
                m = 0x80 >> bit
                c = (1 if b0 & m else 0) | (2 if b1 & m else 0) | (4 if b2 & m else 0) | (8 if b3 & m else 0)
                px[xb * 8 + bit, y] = pal[c]
    im.save(opt.gfx)
print('ports:', {hex(k): v for k, v in sorted(port_log.items())})
print('last:', {hex(k): hex(v) for k, v in sorted(port_last.items()) if k in (0xA2, 0xA4, 0xA6, 0x6A, 0x68)})
