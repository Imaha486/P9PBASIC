/* gfxmirror_test.c - unit checks for the interpreter-to-GVRAM
 * write-through (hal_gpset / hal_gclear vs the vessel's bit-packed
 * planes). No guest runs: the interpreter HAL is called directly and
 * the vessel planes are asserted bit by bit, including the erase
 * case (color 0 must clear what was set), the 200-line gate, colors
 * above 7, display-page resync and the composited hal_gpoint.
 * Exit 0 = all checks pass. */
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "hal.h"
#include "halp8.h"
#include "vessel.h"

static int fails;
static void chk(const char *what, long got, long want)
{
    if (got != want) { printf("FAIL %s: got %ld want %ld\n", what, got, want); fails++; }
}
static int plane_bit(int p, int x, int y)
{
    const unsigned char *pl = vessel_vram_plane(p);
    return (pl[(y >> 1) * 80 + (x >> 3)] >> (7 - (x & 7))) & 1;
}

int main(void)
{
    P8Engine *e = p8_create("/nonexistent.d88");
    const int off = 37 * 80 + 11;       /* sampled plane offset */
    const int nb = (20 >> 1) * 80 + (10 >> 3);  /* byte holding x=10,11 on y=20 */
    int p;
    if (!e) { printf("FAIL create\n"); return 1; }

    /* mirroring is gated on the interpreter's 200-line modes */
    hal_gfx200(1);

    /* gate off: the interpreter surface is written, the vessel is not */
    hal_gfx200(0);
    hal_gpset(10, 20, 7);
    for (p = 0; p < 3; p++) chk("gate off byte", vessel_vram_plane(p)[nb], 0x00);
    hal_gfx200(1);

    /* color 5 = binary 101: B and G set, R clear */
    hal_gpset(10, 20, 5);
    chk("pset5 plane0", plane_bit(0, 10, 20), 1);
    chk("pset5 plane1", plane_bit(1, 10, 20), 0);
    chk("pset5 plane2", plane_bit(2, 10, 20), 1);
    chk("pset5 byte", vessel_vram_plane(0)[(20 >> 1) * 80 + (10 >> 3)], 0x20);
    chk("pset5 gpoint", hal_gpoint(10, 20), 5);

    /* the erase case: color 0 clears all three plane bits */
    hal_gpset(10, 20, 0);
    chk("pset0 plane0", plane_bit(0, 10, 20), 0);
    chk("pset0 plane1", plane_bit(1, 10, 20), 0);
    chk("pset0 plane2", plane_bit(2, 10, 20), 0);
    chk("pset0 gpoint", hal_gpoint(10, 20), 0);

    /* erasing x=11 must leave its byte neighbour x=10 alone */
    hal_gpset(10, 20, 7);
    hal_gpset(11, 20, 0);
    for (p = 0; p < 3; p++) {
        chk("neighbor bit5 kept", (vessel_vram_plane(p)[nb] >> 5) & 1, 1);
        chk("neighbor bit4 clear", (vessel_vram_plane(p)[nb] >> 4) & 1, 0);
    }
    hal_gpset(10, 20, 0);

    /* out-of-range writes are ignored */
    hal_gpset(640, 20, 7); hal_gpset(10, 400, 7); hal_gpset(-1, -1, 7);
    chk("oob plane0 first", vessel_vram_plane(0)[0], 0x00);
    chk("oob plane0 last", vessel_vram_plane(0)[199 * 80 + 79], 0x00);

    /* the vessel's own bounds checks: x=640 would wrap to offset 880,
     * y=400 would land at 16001 */
    vessel_vram_pset(640, 20, 7);
    vessel_vram_pset(10, 400, 7);
    for (p = 0; p < 3; p++) {
        chk("vessel oob x", vessel_vram_plane(p)[880], 0x00);
        chk("vessel oob y", vessel_vram_plane(p)[16001], 0x00);
    }

    /* colors above 7 clear the vessel bits; the interpreter color shows */
    hal_gclear(0);
    hal_gpset(20, 20, 9);
    for (p = 0; p < 3; p++) chk("pset9 plane", plane_bit(p, 20, 20), 0);
    chk("pset9 gpoint", hal_gpoint(20, 20), 9);
    hal_gclear(9);
    for (p = 0; p < 3; p++) {
        chk("clear9 plane", vessel_vram_plane(p)[off], 0x00);
        chk("clear9 plane pixel", vessel_vram_plane(p)[(20 >> 1) * 80 + (20 >> 3)], 0x00);
    }
    chk("clear9 gpoint", hal_gpoint(20, 20), 9);

    /* a guest-drawn pixel (vessel only) is visible to hal_gpoint */
    hal_gclear(0);
    vessel_vram_pset(30, 40, 5);
    chk("guest gpoint", hal_gpoint(30, 40), 5);

    /* display-page changes resync the single GVRAM from the new page */
    hal_gpage(0, 0);
    hal_gclear(0);
    hal_gpset(40, 60, 3);               /* page 0, displayed: mirrored */
    hal_gpage(1, 0);
    hal_gclear(0);                      /* hidden page: no mirror */
    chk("hidden clear plane0", plane_bit(0, 40, 60), 1);
    hal_gpage(1, 1);                    /* blank page 1 now displayed */
    for (p = 0; p < 3; p++) chk("flip to page1 plane", plane_bit(p, 40, 60), 0);
    hal_gpage(0, 0);                    /* back to page 0 */
    chk("flip back plane0", plane_bit(0, 40, 60), 1);
    chk("flip back plane1", plane_bit(1, 40, 60), 1);
    chk("flip back plane2", plane_bit(2, 40, 60), 0);

    /* mode-change clear (graph.c SCREEN handler sequence): leaving
     * a 200-line mode, the handler enables the mirror for the clear
     * before setting the new mode's flag, so mirrored pixels do not
     * survive into the 400-line mode */
    hal_gpset(50, 80, 3);               /* mirrored in 200-line mode */
    chk("modechg pre plane0", plane_bit(0, 50, 80), 1);
    hal_gfx200(1);                      /* was200 || to200: on for the clear */
    hal_gpage(0, 0);
    hal_gclear(0);                      /* reaches the vessel GVRAM */
    hal_gfx200(0);                      /* the new (400-line) mode's state */
    chk("modechg clear plane0", plane_bit(0, 50, 80), 0);
    chk("modechg clear plane1", plane_bit(1, 50, 80), 0);
    hal_gfx200(1);                      /* restore for the checks below */

    /* color 3 = binary 011: B and R filled, G empty */
    hal_gclear(3);
    chk("clear3 plane0", vessel_vram_plane(0)[off], 0xFF);
    chk("clear3 plane1", vessel_vram_plane(1)[off], 0xFF);
    chk("clear3 plane2", vessel_vram_plane(2)[off], 0x00);
    chk("clear3 gpoint", hal_gpoint(300, 75), 3);

    /* the fill covers the visible 80x200 bytes only, not the tail */
    hal_gclear(7);
    for (p = 0; p < 3; p++) {
        chk("clear7 last visible", vessel_vram_plane(p)[15999], 0xFF);
        chk("clear7 tail untouched", vessel_vram_plane(p)[16000], 0x00);
    }

    hal_gclear(0);
    chk("clear0 plane0", vessel_vram_plane(0)[off], 0x00);
    chk("clear0 plane1", vessel_vram_plane(1)[off], 0x00);
    chk("clear0 plane2", vessel_vram_plane(2)[off], 0x00);
    chk("clear0 last visible", vessel_vram_plane(0)[15999], 0x00);
    chk("clear0 gpoint", hal_gpoint(300, 75), 0);

    if (fails) { printf("gfxmirror: %d FAIL\n", fails); return 1; }
    printf("gfxmirror ok\n");
    return 0;
}
