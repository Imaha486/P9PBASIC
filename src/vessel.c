/* vessel.c - Z80 "vessel": runs BLOAD ,R machine code on an emulated Z80
 *   Memory is the interpreter's 64KB guest memory (hal_peek/hal_poke, seg 0).
 *   PC-88 ROM entry points are trapped by address and emulated or skipped.
 *   I/O ports model the PC-88 disk subsystem (8255 at FCh-FFh, D88 image
 *   from P8P_DISK), the OPNA register file with its Timer B, and the
 *   IM 2 interrupt pair (VRTC + Timer B) that PMD's playback runs on.
 *   Trace output (stderr) is enabled by the P8P_TRACE_VESSEL environment variable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal.h"
#include "vessel.h"
#include "opna.h"
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
static unsigned char misc_port[256];

/* OPNA (YM2608) register file. Both PC-88 port pairs are tracked:
 * 44h/45h + 46h/47h and A8h-A9h + AAh/ABh (bank 0 / bank 1).
 * Bank 0 registers 26h (Timer B counter) and 27h (timer control)
 * drive the interrupt model below. */
static int opn_addr[2];
static unsigned char opn_reg[2][256];
static unsigned long opn_nwrite[256];   /* bank-0 write counts */
static unsigned long opn_nwrite1[256];  /* bank-1 write counts */
static int tb_flag = 0;                 /* Timer B status flag */

static void opn_write(int bank, int addr, int v)
{
    opn_reg[bank][addr] = (unsigned char)v;
    opna_write(bank, addr, v);          /* sound rendering sink (ymfm) */
    if (bank == 1) { opn_nwrite1[addr]++; return; }
    if (bank == 0) {
        opn_nwrite[addr]++;
        if (addr == 0x27 && (v & 0x08)) tb_flag = 0;    /* RST B */
        if (tracing() && (addr == 0x26 || addr == 0x27))
            fprintf(stderr, "VESSEL OPN reg %02X <- %02X\n", addr, v);
    }
}

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
    case 0x44: case 0xA8: return tb_flag ? 0x02 : 0x00;  /* OPNA status: never busy */
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
    case 0x44: case 0xA8: opn_addr[0] = v; break;
    case 0x45: case 0xA9: opn_write(0, opn_addr[0], v); break;
    case 0x46: case 0xAA: opn_addr[1] = v; break;
    case 0x47: case 0xAB: opn_write(1, opn_addr[1], v); break;
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

/* Read a numeric literal from the token stream at HL (the forms the
 * PC-88 tokenizer produces): digit tokens / ASCII digits, 0x0F int8,
 * 0x1C int16, 0x0C / 0x0B u16. Returns the value; *php advances. */
static long stream_number(unsigned *php)
{
    unsigned p = *php;
    long v = 0;
    int neg = 0, any = 0;
    while (hal_peek(0, p) == 0x20) p++;
    if (hal_peek(0, p) == 0x2D) { neg = 1; p++; }
    for (;;) {
        int b = hal_peek(0, p);
        if (b >= 0x11 && b <= 0x1A) { v = v * 10 + (b - 0x11); p++; any = 1; }
        else if (b >= '0' && b <= '9') { v = v * 10 + (b - '0'); p++; any = 1; }
        else break;
    }
    if (!any) {
        int b = hal_peek(0, p);
        if (b == 0x0F) { v = (signed char)hal_peek(0, p + 1); p += 2; }
        else if (b == 0x1C) { v = (short)(hal_peek(0, p + 1) | (hal_peek(0, p + 2) << 8)); p += 3; }
        else if (b == 0x0C || b == 0x0B) { v = hal_peek(0, p + 1) | (hal_peek(0, p + 2) << 8); p += 3; }
    }
    *php = p & 0xFFFF;
    return neg ? -v : v;
}

/* check PC traps: -1 = stop, 1 = handled (PC changed), 0 = execute normally */
static int trap(void)
{
    unsigned pc = cpu.pc, ret;
    if (pc == SENTINEL) return -1;
    if (pc == 0x0008) {     /* RST 8: inline service byte follows the RST */
        ret = rd16(cpu.sp);
        if (tracing()) { fprintf(stderr, "VESSEL RST8 svc %02X at %04X", hal_peek(0, ret), ret); trace_regs(); }
        if (hal_peek(0, ret) == 0x2C) {
            /* svc 0x2C: skip the separator at HL, evaluate the next
             * argument and leave its descriptor at (0xEC41):
             * [type, lo, hi]. The driver's readers fetch it from there. */
            unsigned p = (unsigned)cpu.h << 8 | cpu.l;
            long v;
            while (hal_peek(0, p) == 0x20) p++;
            if (hal_peek(0, p) == 0x2C) p++;
            while (hal_peek(0, p) == 0x20) p++;
            v = stream_number(&p);
            hal_poke(0, 0xEC44, 2);
            hal_poke(0, 0xEC45, (int)(v & 0xFF));
            hal_poke(0, 0xEC46, (int)((v >> 8) & 0xFF));
            cpu.h = (uint8_t)(p >> 8); cpu.l = (uint8_t)p;
        }
        cpu.sp = (uint16_t)(cpu.sp + 2);
        cpu.pc = (uint16_t)((ret + 1) & 0xFFFF);
        return 1;
    }
    if (pc == 0x18A3) {     /* ROM: fetch the evaluated value's low byte -> A */
        cpu.a = (uint8_t)hal_peek(0, 0xEC45);
        cpu.e = (uint8_t)hal_peek(0, 0xEC45);
        cpu.d = (uint8_t)hal_peek(0, 0xEC46);
        emu_ret();
        return 1;
    }
    if (pc == 0xA794) {     /* NEOMSC's argument fetch (a slot the ROM's
                             * registration call patches on hardware):
                             * DE = the evaluated 16-bit argument. */
        cpu.e = (uint8_t)hal_peek(0, 0xEC45);
        cpu.d = (uint8_t)hal_peek(0, 0xEC46);
        emu_ret();
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

/* ================= interrupts (PMD's IM 2 pair) ================= */
/* PMD88 (KAJA) runs on two interrupts, installed by its init (rtlset)
 * into the Z80 IM 2 vector table at (I << 8): VRTC at +2 (fadeout and
 * effects, ~60 Hz; the handler chains to BASIC's VRTC handler at
 * 0xE808) and the OPNA interrupt at +8 (the sequencer main, clocked by
 * Timer B: counter register 0x26, control 0x27). On hardware the host
 * BASIC owns the I register and the table page; the vessel presets I
 * to a free page and delivers both interrupts through z80_gen_int.
 * Layout per the PMD88 v3.7 source (src2.txt L35-36, L111-223,
 * L225-267, L476-523); MA2026 bundles PMD v3.3, so the harness
 * verifies that the bundled binary registers the same slots. */
#define IRQ_PAGE    0xE9
#define VRTC_VEC    2
#define OPN_VEC     8
#define REST_CELL   0xE80A      /* planted RET: the at-rest mainline PC */
#define VRTC_PERIOD 66667L      /* 4 MHz CPU / 60 Hz */
#define PUMP_SLICE  16667L      /* emulated cycles per vessel_pump() */

static int in_run = 0;
static unsigned long last_cyc = 0;
static long vrtc_left = VRTC_PERIOD;
static long tb_left = 0;
static int vrtc_due = 0, tb_due = 0;
static int slot_vrtc_seen = 0, slot_opn_seen = 0;

static long tb_period(void)
{
    /* Timer B period in 4 MHz CPU cycles: (256 - reg26) counts of
     * 1152 OPNA clocks at 7.9872 MHz. */
    return (long)(256 - opn_reg[0][0x26]) * 1152L * 4000L / 7987L;
}

static void irq_advance(long cycles)
{
    opna_advance(cycles);               /* audio rendering follows the same clock */
    vrtc_left -= cycles;
    if (vrtc_left <= 0) { vrtc_left += VRTC_PERIOD; vrtc_due = 1; }
    if (opn_reg[0][0x27] & 0x02) {              /* Timer B enabled */
        if (tb_left <= 0) tb_left = tb_period();
        tb_left -= cycles;
        if (tb_left <= 0) {
            tb_left += tb_period();
            tb_flag = 1;
            if (opn_reg[0][0x27] & 0x20) tb_due = 1;    /* IRQ enable B */
        }
    }
}

/* Queue at most one interrupt with the CPU core (Timer B first). */
static void irq_service(void)
{
    unsigned h;
    if (cpu.int_pending) return;
    if (tb_due) {
        tb_due = 0;
        h = rd16((IRQ_PAGE << 8) | OPN_VEC);
        if (h >= 0x8000) {
            if (!slot_opn_seen && tracing()) fprintf(stderr, "VESSEL OPN int handler %04X\n", h);
            slot_opn_seen = 1;
            z80_gen_int(&cpu, OPN_VEC);
            return;
        }
    }
    if (vrtc_due) {
        vrtc_due = 0;
        h = rd16((IRQ_PAGE << 8) | VRTC_VEC);
        if (h >= 0x8000) {
            if (!slot_vrtc_seen && tracing()) fprintf(stderr, "VESSEL VRTC int handler %04X\n", h);
            slot_vrtc_seen = 1;
            z80_gen_int(&cpu, VRTC_VEC);
        }
    }
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
    /* PMD's init reads the I register to find the IM 2 vector table;
     * preset it (and IM 2) the way the host BASIC would. BASIC's VRTC
     * handler at 0xE808 gets an EI;RET stub for the handler chain. */
    cpu.i = IRQ_PAGE;
    cpu.interrupt_mode = 2;
    hal_poke(0, 0xE808, 0xFB);
    hal_poke(0, 0xE809, 0xC9);
    hal_poke(0, REST_CELL, 0xC9);
    /* The interpreter's evaluated-argument descriptor lives at (0xEC41);
     * point it at a scratch block the RST 8 trap fills in. */
    hal_poke(0, 0xEC41, 0x44);
    hal_poke(0, 0xEC42, 0xEC);
    hal_poke(0, 0xEC44, 0);
    hal_poke(0, 0xEC45, 0);
    hal_poke(0, 0xEC46, 0);
}

static long run_loop_body(void)
{
    long n;
    int t;
    int steps = getenv("P8P_TRACE_STEPS") != 0;
    last_cyc = cpu.cyc;
    for (n = 0; n < STEP_CAP; n++) {
        t = trap();
        if (t < 0) return n;
        if (t > 0) continue;
        if (steps) fprintf(stderr, "STEP %04X %02X\n", (unsigned)cpu.pc, hal_peek(0, cpu.pc));
        z80_step(&cpu);
        {   /* emulated time follows the CPU's own cycle counter */
            unsigned long now = cpu.cyc;
            long dc = (long)(now - last_cyc);
            last_cyc = now;
            if (dc > 0) irq_advance(dc);
        }
        irq_service();
        if (cpu.halted) {
            fprintf(stderr, "VESSEL HALT at %04X, stopped\n", (unsigned)cpu.pc);
            return n;
        }
    }
    fprintf(stderr, "VESSEL step cap reached at %04X, stopped\n", (unsigned)cpu.pc);
    return -1;
}

static long run_loop(void)
{
    long r;
    in_run = 1;
    r = run_loop_body();
    in_run = 0;
    return r;
}

/* Advance emulated time while the native interpreter runs (BASIC is
 * the mainline; the Z80 only runs in bursts), and execute any
 * interrupt handlers that come due, on a sentinel stack like a call.
 * Called once per statement from exec_loop. */
void vessel_pump(void)
{
    int k;
    static unsigned long pump_count = 0;
    if (!inited || in_run) return;
    /* At rest the machine belongs to the host mainline, which runs
     * with interrupts enabled (the ROM does); a driver call that
     * returns with DI must not mute the machine for good. */
    cpu.iff1 = 1;
    cpu.iff2 = 1;
    if (tracing() && ++pump_count % 60000 == 0) {
        unsigned long tot = 0;
        int i;
        for (i = 0; i < 256; i++) tot += opn_nwrite[i];
        fprintf(stderr, "VESSEL pump %lu: OPN writes total %lu, keyon/off %lu, timerB %lu, iff1 %d pend %d\n",
                pump_count, tot, opn_nwrite[0x28], opn_nwrite[0x26],
                (int)cpu.iff1, (int)cpu.int_pending);
    }
    irq_advance(PUMP_SLICE);
    /* Step the CPU whenever an interrupt is due OR already pending:
     * a vector queued at the tail of a driver call is only accepted
     * by a later step, so refusing to step while one is pending
     * would wedge playback for good. */
    for (k = 0; k < 2 && (vrtc_due || tb_due || cpu.int_pending); k++) {
        cpu.halted = 0;
        cpu.sp = (uint16_t)(STACK_INIT - 2);
        hal_poke(0, cpu.sp, SENTINEL & 0xFF);
        hal_poke(0, cpu.sp + 1, SENTINEL >> 8);
        cpu.pc = (uint16_t)REST_CELL;   /* steppable rest PC: a pending
                                         * interrupt is accepted before
                                         * its RET reaches the sentinel */
        irq_service();
        run_loop();
        if (!cpu.int_pending && !vrtc_due && !tb_due) break;
    }
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

/* FFDE service 0 equivalent: read count 256-byte sectors starting at
 * (logical track, sector) from the P8P_DISK image into guest memory.
 * Logical track = cylinder * 2 + head, sector = 1..16 (D88 order).
 * Returns the number of sectors read. */
int vessel_disk_read(int ltrack, int sector, int count, unsigned addr)
{
    unsigned char buf[256];
    int i, t = ltrack, r = sector;
    for (i = 0; i < count; i++) {
        int j;
        if (!d88_read_sector(t, r, buf)) break;
        for (j = 0; j < 256; j++) hal_poke(0, (addr + (unsigned)i * 256u + (unsigned)j) & 0xFFFF, buf[j]);
        if (++r > 16) { r = 1; t++; }
    }
    if (tracing()) fprintf(stderr, "VESSEL disk_read track %d sec %d count %d -> %04X (%d read)\n",
                           ltrack, sector, count, addr, i);
    if (i > 0) vessel_notify_load(addr, (long)i * 256L);
    return i;
}

unsigned long vessel_opn_keyons(void)
{
    return opn_nwrite[0x28];
}

unsigned long vessel_opn_writes(void)
{
    unsigned long tot = 0;
    int i;
    for (i = 0; i < 256; i++) tot += opn_nwrite[i];
    return tot;
}

unsigned long vessel_opn_writes1(void)
{
    unsigned long tot = 0;
    int i;
    for (i = 0; i < 256; i++) tot += opn_nwrite1[i];
    return tot;
}

unsigned long vessel_opn_adpcmb_ctl(void)
{
    return opn_nwrite[0x10];    /* ADPCM-B control register writes */
}

unsigned long vessel_opn_rhythm_writes(void)
{
    unsigned long tot = 0;
    int i;
    for (i = 0; i < 0x30; i++) tot += opn_nwrite1[i];   /* ADPCM-A block */
    return tot;
}

void vessel_notify_load(unsigned addr, long len)
{
    addr &= 0xFFFF;
    if (len > 0 && addr <= PMD_START && (long)addr + len > (long)PMD_START) pmd_loaded = 1;
    if (tracing()) fprintf(stderr, "VESSEL load %04X len %ld%s\n", addr, len, pmd_loaded ? " (PMD loaded)" : "");
}
