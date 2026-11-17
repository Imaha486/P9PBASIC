/* vessel.h - Z80 "vessel": runs BLOAD ,R machine code on an emulated Z80
 *   The Z80 shares the interpreter's 64KB guest memory (hal_peek/hal_poke).
 *   CPU core: third_party/z80 (superzazu/z80, MIT).
 */
#ifndef VESSEL_H
#define VESSEL_H

void vessel_init(void);                            /* idempotent one-time setup */
void vessel_exec(unsigned entry);                  /* run Z80 from entry until it RETs to a sentinel */
void vessel_notify_load(unsigned addr, long len);  /* called after BLOAD loads a block */

#endif
