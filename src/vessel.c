/* vessel.c - Z80 "vessel": runs BLOAD ,R machine code on an emulated Z80
 *   Memory is the interpreter's 64KB guest memory (hal_peek/hal_poke, seg 0).
 *   PC-88 ROM entry points are trapped by address and emulated or skipped.
 *   I/O ports model the PC-88 disk subsystem (8255 at FCh-FFh, D88 image
 *   from P8P_DISK) plus a few sound board / CTC stubs.
 *   Trace output (stderr) is enabled by the P8P_TRACE_VESSEL environment variable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal.h"
#include "vessel.h"
#include "z80.h"

#define STEP_CAP    4000000L
#define STACK_INIT  0xF000u
#define SENTINEL    0xFFFFu
#define PMD_START   0xAA00u
#define PMD_END     0xAFFFu

static z80 cpu;
static int inited = 0;
static int vtrace = -1;
static int pmd_loaded = 0;
static unsigned char rom_seen[0x8000 / 8];
static unsigned char pmd_seen[(PMD_END - PMD_START + 1 + 7) / 8];
static unsigned char in_seen[256 / 8], out_seen[256 / 8];

static int tracing(void)
{
    if (vtrace < 0) vtrace = getenv("P8P_TRACE_VESSEL") != 0;
    return vtrace;
}

/* returns 1 the first time bit n of map is tested */
static int first_time(unsigned char *map, unsigned n)
{
    if (map[n >> 3] & (1 << (n & 7))) return 0;
    map[n >> 3] |= (unsigned char)(1 << (n & 7));
    return 1;
}

static unsigned rd16(unsigned a)
{
    return (unsigned)hal_peek(0, a & 0xFFFF) | ((unsigned)hal_peek(0, (a + 1) & 0xFFFF) << 8);
}

static void trace_regs(void)
{
    fprintf(stderr, " A=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X SP=%04X\n",
        cpu.a, cpu.b, cpu.c, cpu.d, cpu.e, cpu.h, cpu.l, (unsigned)cpu.sp);
}

/* ================= disk image (D88) ================= */

#define D88_MAX 0x200000L
static unsigned char d88[D88_MAX];
static long d88_size = 0;
static int d88_state = 0;   /* 0 = not tried, 1 = ok, -1 = unavailable */

static int d88_open(void)
{
    const char *fn;
    FILE *fp;
    if (d88_state) return d88_state > 0;
    d88_state = -1;
    fn = getenv("P8P_DISK");
    if (!fn) return 0;
    fp = fopen(fn, "rb");
    if (!fp) { if (tracing()) fprintf(stderr, "SUBSYS cannot open %s\n", fn); return 0; }
    d88_size = (long)fread(d88, 1, (size_t)D88_MAX, fp);
    fclose(fp);
    if (d88_size < 0x20 + 164 * 4) return 0;
    d88_state = 1;
    return 1;
}

static unsigned d88_u16(long p)
{
    return (unsigned)d88[p] | ((unsigned)d88[p + 1] << 8);
}

static unsigned long d88_u32(long p)
{
    return (unsigned long)d88_u16(p) | ((unsigned long)d88_u16(p + 2) << 16);
}

/* copy sector r of subsystem track t (256 bytes) to dst; 0 on error */
static int d88_read_sector(int t, int r, unsigned char *dst)
{
    int cyl, head, n, count;
    unsigned long block;
    long pos;
    unsigned dsize, cp;
    if (!d88_open()) return 0;
    if (t < 0 || t >= 164) return 0;
    cyl = t >> 1; head = t & 1;
    block = d88_u32(0x20 + (long)(cyl * 2 + head) * 4);
    if (block == 0 || block + 16 > (unsigned long)d88_size) return 0;
    pos = (long)block;
    count = (int)d88_u16(pos + 4);   /* sectors in this track */
    if (count <= 0 || count > 64) count = 64;
    for (n = 0; n < count && pos + 16 <= d88_size; n++) {
        dsize = d88_u16(pos + 0x0E);
        if (dsize == 0) dsize = 128u << (d88[pos + 3] & 7);
        if (d88[pos + 2] == r) {
            cp = dsize < 256 ? dsize : 256;
            if (pos + 16 + (long)cp > d88_size) return 0;
            memset(dst, 0, 256);
            memcpy(dst, d88 + pos + 16, cp);
            return 1;
        }
        pos += 16 + (long)dsize;
    }
    return 0;
}

/* ================= disk subsystem command processor ================= */

/* output queue holds a whole read buffer plus a few status bytes */
#define OQ_SIZE 0x4400u
static unsigned char outq[OQ_SIZE];
static unsigned oq_head = 0, oq_count = 0;

static unsigned char read_buf[16384];
static unsigned read_len = 0;
static int read_valid = 0;

static int sub_cmd = -1;        /* current command, -1 = idle */
static int sub_need = 0, sub_npar = 0;
static int sub_par[4];
static long sub_wleft = 0;      /* write data bytes still expected */
static int sub_err = 0;
static int surface_mode = 0;

static void oq_push(int v)
{
    if (oq_count < OQ_SIZE) { outq[(oq_head + oq_count) % OQ_SIZE] = (unsigned char)v; oq_count++; }
}

static int oq_pop(void)
{
    int v;
    if (!oq_count) return 0xFF;
    v = outq[oq_head];
    oq_head = (oq_head + 1) % OQ_SIZE; oq_count--;
    return v;
}

static void subsys_read(void)
{
    int nsec = sub_par[0], t = sub_par[2], r = sub_par[3], k;
    read_len = 0; read_valid = 0;
    if (nsec > (int)(sizeof(read_buf) / 256)) nsec = (int)(sizeof(read_buf) / 256);
    for (k = 0; k < nsec; k++) {
        if (!d88_read_sector(t, r, read_buf + (unsigned)k * 256)) { sub_err = 1; return; }
        read_len += 256;
        if (++r > 16) { r = 1; t++; }
    }
    read_valid = 1; sub_err = 0;
}

static void subsys_complete(void)
{
    unsigned k;
    switch (sub_cmd) {
    case 0x00: sub_err = 0; break;
    case 0x01: sub_err = 0; break;                 /* write: data discarded */
    case 0x02: subsys_read(); break;
    case 0x03: case 0x12:
        for (k = 0; k < read_len; k++) oq_push(read_buf[k]);
        break;
    case 0x06:
        oq_push(0x80 | (read_valid ? 0x40 : 0) | (sub_err ? 1 : 0));
        sub_err = 0;
        break;
    case 0x07: oq_push(0x80); break;
    case 0x17: surface_mode = sub_par[0]; break;
    case 0x18: oq_push(surface_mode); break;
    case 0x23: oq_push(0x00); break;
    }
    if (tracing()) {
        int i;
        fprintf(stderr, "SUBSYS cmd %02X", sub_cmd);
        for (i = 0; i < sub_need; i++) fprintf(stderr, " %02X", sub_par[i]);
        if (sub_cmd == 0x02) fprintf(stderr, " -> %s len %u", read_valid ? "ok" : "error", read_len);
        fprintf(stderr, " (out queue %u)\n", oq_count);
    }
    sub_cmd = -1;
}

static void subsys_command(int b)
{
    sub_cmd = b; sub_npar = 0; sub_wleft = 0;
    switch (b) {
    case 0x01: case 0x02: sub_need = 4; break;
    case 0x17: case 0x23: sub_need = 1; break;
    case 0x00: case 0x03: case 0x06: case 0x07: case 0x12: case 0x18: sub_need = 0; break;
    default:
        if (tracing()) fprintf(stderr, "SUBSYS unknown cmd %02X\n", b);
        sub_need = 0; sub_err = 1; sub_cmd = -1;
        return;
    }
    if (sub_need == 0) subsys_complete();
}

static void subsys_data(int b)
{
    if (sub_cmd < 0) {
        if (tracing()) fprintf(stderr, "SUBSYS stray data %02X\n", b);
        return;
    }
    if (sub_npar < sub_need) {
        sub_par[sub_npar++] = b;
        if (sub_npar == sub_need) {
            if (sub_cmd == 0x01) { sub_wleft = (long)sub_par[0] * 256; if (sub_wleft) return; }
            subsys_complete();
        }
        return;
    }
    if (sub_wleft > 0 && --sub_wleft == 0) subsys_complete();
}

/* ================= I/O ports ================= */

/* 8255 port C bits driven by the host (written via FEh or FFh bit set/reset) */
#define C_ATN 0x80
#define C_DAC 0x40
#define C_RFD 0x20
#define C_DAV 0x10

static int cfg_a_input = 1, cfg_b_input = 0;
static int ppi_mode = 0;
static int hostc = 0;           /* host control bits (bits 4-7) */
static int prev_c = 0;          /* hostc as of the previous pump */
static int latch = 0xFF;        /* last byte written by the host */
static int latched = 0;
static int sub_dac = 0;         /* subsystem DAC status */
static int snd_addr = 0;
static unsigned char misc_port[256];

/* advance the handshake on DAV / DAC edges since the previous call */
static void subsys_pump(void)
{
    int rise = hostc & ~prev_c, fall = ~hostc & prev_c;
    if (rise & C_DAV) {
        if (latched) {
            if (hostc & C_ATN) subsys_command(latch); else subsys_data(latch);
            latched = 0;
        }
        sub_dac = 1;
    }
    if (fall & C_DAV) sub_dac = 0;
    if ((rise & C_DAC) && oq_count) oq_pop();
    prev_c = hostc;
}

static int ppi_status(void)
{
    int dav = oq_count > 0 && !(hostc & C_DAC);
    int rfd = 1;    /* bytes are processed synchronously: input never full */
    return (hostc & 0xF0) | (sub_dac << 2) | (rfd << 1) | dav;
}

static void note_port(unsigned char *map, const char *dir, int port, int v)
{
    if (tracing() && first_time(map, (unsigned)port)) {
        if (v >= 0) fprintf(stderr, "VESSEL %s %02X <- %02X\n", dir, port, v);
        else fprintf(stderr, "VESSEL %s %02X\n", dir, port);
    }
}

int vessel_port_in(int port)
{
    if (port >= 0xFC) {
        subsys_pump();
        switch (port) {
        case 0xFC: return cfg_a_input ? (oq_count ? outq[oq_head] : 0xFF) : latch;
        case 0xFD: return latch;
        case 0xFE: return ppi_status();
        default: return 0xFF;
        }
    }
    note_port(in_seen, "IN", port, -1);
    switch (port) {
    case 0xA9: return 1;    /* Sound Board II present */
    case 0x31: case 0x32: case 0x34: case 0x35: return misc_port[port];
    }
    return 0xFF;
}

void vessel_port_out(int port, int v)
{
    if (port >= 0xFC) {
        subsys_pump();
        switch (port) {
        case 0xFC: if (!cfg_a_input) { latch = v; latched = 1; } break;
        case 0xFD: latch = v; latched = 1; break;
        case 0xFE: hostc = v & 0xF0; break;
        case 0xFF:
            if (v & 0x80) {
                ppi_mode = v;
                cfg_a_input = (v >> 4) & 1;
                cfg_b_input = (v >> 1) & 1;
            } else {
                int bit = (v >> 1) & 7;
                if (bit >= 4) {
                    if (v & 1) hostc |= 1 << bit; else hostc &= ~(1 << bit);
                }
            }
            break;
        }
        subsys_pump();
        return;
    }
    note_port(out_seen, "OUT", port, v);
    switch (port) {
    case 0xA8: snd_addr = v; break;
    case 0x31: case 0x32: case 0x34: case 0x35: misc_port[port] = (unsigned char)v; break;
    }
}

/* ================= CPU ================= */

static uint8_t mem_read(void *ud, uint16_t addr)
{
    (void)ud;
    return (uint8_t)hal_peek(0, addr);
}

static void mem_write(void *ud, uint16_t addr, uint8_t v)
{
    (void)ud;
    hal_poke(0, addr, v);
}

static uint8_t io_in(z80 *z, uint8_t port)
{
    (void)z;
    return (uint8_t)vessel_port_in(port);
}

static void io_out(z80 *z, uint8_t port, uint8_t v)
{
    (void)z;
    vessel_port_out(port, v);
}

static void emu_ret(void)
{
    cpu.pc = (uint16_t)rd16(cpu.sp);
    cpu.sp = (uint16_t)(cpu.sp + 2);
}

/* check PC traps: -1 = stop, 1 = handled (PC changed), 0 = execute normally */
static int trap(void)
{
    unsigned pc = cpu.pc, ret;
    if (pc == SENTINEL) return -1;
    if (pc == 0x0008) {     /* RST 8: inline service byte follows the RST */
        ret = rd16(cpu.sp);
        if (tracing()) { fprintf(stderr, "VESSEL RST8 svc %02X at %04X", hal_peek(0, ret), ret); trace_regs(); }
        cpu.sp = (uint16_t)(cpu.sp + 2);
        cpu.pc = (uint16_t)((ret + 1) & 0xFFFF);
        return 1;
    }
    if (pc == 0x37C9 || pc == 0x37CC) { subsys_command(cpu.a); emu_ret(); return 1; }  /* PUTCOM */
    if (pc == 0x37D2) { subsys_data(cpu.a); emu_ret(); return 1; }                      /* PUTDAT */
    if (pc == 0x3847) { cpu.a = (uint8_t)oq_pop(); emu_ret(); return 1; }               /* GETDAT */
    if (pc == 0x6F6B) {
        if (tracing()) { fprintf(stderr, "VESSEL ROM 6F6B (registration)"); trace_regs(); }
        emu_ret();
        return 1;
    }
    if (pc < 0x8000) {
        if (tracing() && first_time(rom_seen, pc)) { fprintf(stderr, "VESSEL ROM %04X skipped from %04X", pc, rd16(cpu.sp)); trace_regs(); }
        emu_ret();
        return 1;
    }
    if (pc >= PMD_START && pc <= PMD_END && !pmd_loaded) {
        if (tracing() && first_time(pmd_seen, pc - PMD_START)) fprintf(stderr, "VESSEL PMD %04X not loaded, skipped\n", pc);
        emu_ret();
        return 1;
    }
    return 0;
}

void vessel_init(void)
{
    if (inited) return;
    inited = 1;
    z80_init(&cpu);
    cpu.read_byte = mem_read;
    cpu.write_byte = mem_write;
    cpu.port_in = io_in;
    cpu.port_out = io_out;
    cpu.userdata = 0;
}

static long run_loop(void)
{
    long n;
    int t;
    int steps = getenv("P8P_TRACE_STEPS") != 0;
    for (n = 0; n < STEP_CAP; n++) {
        t = trap();
        if (t < 0) return n;
        if (t > 0) continue;
        if (steps) fprintf(stderr, "STEP %04X %02X\n", (unsigned)cpu.pc, hal_peek(0, cpu.pc));
        z80_step(&cpu);
        if (cpu.halted) {
            fprintf(stderr, "VESSEL HALT at %04X, stopped\n", (unsigned)cpu.pc);
            return n;
        }
    }
    fprintf(stderr, "VESSEL step cap reached at %04X, stopped\n", (unsigned)cpu.pc);
    return -1;
}

void vessel_exec(unsigned entry)
{
    long n;
    vessel_init();
    cpu.halted = 0;
    cpu.sp = (uint16_t)(STACK_INIT - 2);
    hal_poke(0, cpu.sp, SENTINEL & 0xFF);
    hal_poke(0, cpu.sp + 1, SENTINEL >> 8);
    cpu.pc = (uint16_t)(entry & 0xFFFF);
    if (tracing()) fprintf(stderr, "VESSEL exec %04X\n", entry & 0xFFFF);
    n = run_loop();
    if (tracing() && n >= 0) fprintf(stderr, "VESSEL returned after %ld steps\n", n);
}

/* Call a driver entry with preset registers (hook experiments and,
 * later, the extension statement relay). Returns the step count. */
long vessel_call(unsigned entry, unsigned a, unsigned hl, unsigned bc, unsigned de)
{
    vessel_init();
    cpu.halted = 0;
    cpu.sp = (uint16_t)(STACK_INIT - 2);
    hal_poke(0, cpu.sp, SENTINEL & 0xFF);
    hal_poke(0, cpu.sp + 1, SENTINEL >> 8);
    cpu.a = (uint8_t)a;
    cpu.h = (uint8_t)(hl >> 8); cpu.l = (uint8_t)hl;
    cpu.b = (uint8_t)(bc >> 8); cpu.c = (uint8_t)bc;
    cpu.d = (uint8_t)(de >> 8); cpu.e = (uint8_t)de;
    cpu.pc = (uint16_t)(entry & 0xFFFF);
    if (tracing()) fprintf(stderr, "VESSEL call %04X A=%02X HL=%04X BC=%04X DE=%04X\n",
                               entry & 0xFFFF, a & 0xFF, hl & 0xFFFF, bc & 0xFFFF, de & 0xFFFF);
    return run_loop();
}

void vessel_notify_load(unsigned addr, long len)
{
    addr &= 0xFFFF;
    if (len > 0 && addr <= PMD_START && (long)addr + len > (long)PMD_START) pmd_loaded = 1;
    if (tracing()) fprintf(stderr, "VESSEL load %04X len %ld%s\n", addr, len, pmd_loaded ? " (PMD loaded)" : "");
}
