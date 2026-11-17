/* vessel.h - Z80 "vessel": runs BLOAD ,R machine code on an emulated Z80
 *   The Z80 shares the interpreter's 64KB guest memory (hal_peek/hal_poke).
 *   CPU core: third_party/z80 (superzazu/z80, MIT).
 */
#ifndef VESSEL_H
#define VESSEL_H

void vessel_init(void);                            /* idempotent one-time setup */
void vessel_exec(unsigned entry);                  /* run Z80 from entry until it RETs to a sentinel */
long vessel_call(unsigned entry, unsigned a, unsigned hl, unsigned bc, unsigned de); /* call with preset registers */
void vessel_notify_load(unsigned addr, long len);  /* called after BLOAD loads a block */

/* The PC-88 port model, shared by the Z80 and by BASIC's own INP/OUT
 * (the host HAL delegates hal_inp/hal_outp to these). */
int  vessel_port_in(int port);
void vessel_port_out(int port, int value);

#endif
