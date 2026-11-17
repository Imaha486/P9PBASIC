/* vessel.h - Z80 "vessel": runs BLOAD ,R machine code on an emulated Z80
 *   The Z80 shares the interpreter's 64KB guest memory (hal_peek/hal_poke).
 *   CPU core: third_party/z80 (superzazu/z80, MIT).
 */
#ifndef VESSEL_H
#define VESSEL_H

#ifdef __cplusplus
extern "C" {
#endif

void vessel_init(void);                            /* idempotent one-time setup */
void vessel_exec(unsigned entry);                  /* run Z80 from entry until it RETs to a sentinel */
long vessel_call(unsigned entry, unsigned a, unsigned hl, unsigned bc, unsigned de);
long vessel_call_pushed(unsigned entry, unsigned a, unsigned hl, unsigned bc, unsigned de, unsigned pushed); /* call with preset registers */
int  vessel_d88_sector(int t, int r, unsigned char *dst); /* raw D88 sector (t = cyl*2+head) */
int  vessel_disk_read(int ltrack, int sector, int count, unsigned addr);
int  vessel_dram_load(int ltrack, int tracks); /* FFDE service 0: raw sector read from P8P_DISK */
void vessel_notify_load(unsigned addr, long len);  /* called after BLOAD loads a block */
void vessel_pump(void);                            /* advance emulated time + deliver due interrupts (call while BASIC runs) */
unsigned long vessel_opn_keyons(void);             /* OPNA key on/off write count (diagnostics) */
unsigned long vessel_opn_writes(void);             /* OPNA bank-0 write total (diagnostics) */
unsigned long vessel_opn_writes1(void);            /* OPNA bank-1 write total (diagnostics) */
unsigned long vessel_opn_adpcmb_ctl(void);         /* ADPCM-B control writes (diagnostics) */
unsigned long vessel_opn_class(int lo, int hi);    /* bank-0 write count over a register range */
unsigned long vessel_opn_rhythm_writes(void);      /* ADPCM-A block writes (diagnostics) */

/* The PC-88 port model, shared by the Z80 and by BASIC's own INP/OUT
 * (the host HAL delegates hal_inp/hal_outp to these). */
int  vessel_port_in(int port);
void vessel_port_out(int port, int value);

#ifdef __cplusplus
}
#endif

#endif
