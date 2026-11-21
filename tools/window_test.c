/* window_test.c - unit checks for the vessel's RAM window
 * (I/O 70H base, I/O 31H 64K-RAM mode) on the Z80 bus.
 * A tiny Z80 routine reads and writes through 8000-83FF; the
 * flat memory (what hal_peek sees) is asserted after each call.
 * Exit 0 = all checks pass. */
#include <stdio.h>
#include "vessel.h"
#include "hal.h"

static int fails;
static void chk(const char *what, long got, long want)
{
    if (got != want) { printf("FAIL %s: got %ld want %ld\n", what, got, want); fails++; }
}
static int peek(int a) { return hal_peek(0, a); }
static void poke(int a, int v) { hal_poke(0, a, v); }

int main(void)
{
    /* LD A,(8000H) / LD (9100H),A / LD A,5AH / LD (8001H),A / RET */
    static const unsigned char prog[] = {
        0x3A, 0x00, 0x80, 0x32, 0x00, 0x91, 0x3E, 0x5A, 0x32, 0x01, 0x80, 0xC9
    };
    int i;
    vessel_init();
    for (i = 0; i < (int)sizeof(prog); i++) poke(0x9000 + i, prog[i]);
    poke(0x8000, 0x11); poke(0x8001, 0x00);
    poke(0x4000, 0x22); poke(0x4001, 0x00);

    /* default: base 8000H, 64K-RAM off -> identity */
    vessel_call(0x9000, 0, 0, 0, 0);
    chk("default read", peek(0x9100), 0x11);
    chk("default write identity", peek(0x8001), 0x5A);
    chk("default write no window", peek(0x4001), 0x00);
    poke(0x8001, 0x00); poke(0x9100, 0x00);

    /* OUT 70H,40H: window base 4000H */
    vessel_port_out(0x70, 0x40);
    chk("IN 70H reads base back", vessel_port_in(0x70), 0x40);
    vessel_call(0x9000, 0, 0, 0, 0);
    chk("window read", peek(0x9100), 0x22);
    chk("window write", peek(0x4001), 0x5A);
    chk("window write no identity", peek(0x8001), 0x00);
    poke(0x4001, 0x00); poke(0x9100, 0x00);

    /* OUT 31H,02H: 64K-RAM mode wins over the window */
    vessel_port_out(0x31, 0x02);
    vessel_call(0x9000, 0, 0, 0, 0);
    chk("64ram read", peek(0x9100), 0x11);
    chk("64ram write", peek(0x8001), 0x5A);
    poke(0x8001, 0x00); poke(0x9100, 0x00);

    /* OUT 31H,00H: window active again */
    vessel_port_out(0x31, 0x00);
    vessel_call(0x9000, 0, 0, 0, 0);
    chk("window read again", peek(0x9100), 0x22);

    if (fails) { printf("window: %d FAIL\n", fails); return 1; }
    printf("window ok\n");
    return 0;
}
