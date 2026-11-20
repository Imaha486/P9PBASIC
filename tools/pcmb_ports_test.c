/* pcmb_ports_test.c - unit check for the OPNA bank-1 port pairs.
 * PMD88 drives the ADPCM-B registers through ports 0xAC (address) /
 * 0xAD (data) -- the Sound Board II's A8-AF decode (QUASI88
 * pc88main.cpp cases 0xac/0xad) -- and polls the extended status
 * (EOS, bit 2) by reading port 0xAC. The vessel originally mapped
 * only 0x46/0x47 (the pair NEOMSC itself uses), so every ADPCM-B
 * write from PMD was silently dropped and the PCM part never
 * sounded. This test programs a playback entirely through
 * 0xAC/0xAD and requires the chip to report PLAYING on the
 * extended-status port and to clear it when the sample ends.
 * Exit 0 = all checks pass. */
#include <stdio.h>
#include "vessel.h"
#include "opna.h"

static int fails;
static void chk(const char *what, long got, long want)
{
    if (got != want) { printf("FAIL %s: got %ld want %ld\n", what, got, want); fails++; }
}

static void regw(int addr, int value)   /* one bank-1 write via 0xAC/0xAD */
{
    vessel_port_out(0xAC, addr);
    vessel_port_out(0xAD, value);
}

static void dummy_sink(const short *frames, int count, void *ctx)
{
    (void)frames; (void)count; (void)ctx;
}

int main(void)
{
    long i;
    int playing = 0;

    /* the ymfm wrapper stays inert until a WAV path or an audio sink
     * exists (ensure_init); the embed/app always provide one */
    opna_set_sink(dummy_sink, NULL);
    vessel_init();
    opna_reset();

    /* unmask the ADPCM flags: ymfm's reset leaves the flag-control
     * register at 0x1C, which hides EOS/BRDY/ZERO from
     * read_status_hi(); a guest clears the mask through bank-1
     * register 0x10 (the flag-control register) */
    regw(0x10, 0x00);

    /* sample data in ADPCM-B DRAM (any nonzero pattern) */
    for (i = 0; i < 65536; i++) opna_adpcm_b_write(i, (int)(i * 37 + 11));

    /* PMD's keyon sequence shape (keyonm_00), through 0xAC/0xAD only;
     * a short sample (stop address close) so it ends within the test */
    regw(0x00, 0x21);           /* reset */
    regw(0x01, 0x00);           /* control: DRAM, pan centred below */
    regw(0x02, 0x00);           /* start address L */
    regw(0x03, 0x00);           /* start address H */
    regw(0x04, 0x40);           /* stop address L */
    regw(0x05, 0x00);           /* stop address H */
    regw(0x09, 0x00);           /* delta-N L */
    regw(0x0A, 0x10);           /* delta-N H (nominal rate) */
    regw(0x0B, 0xFF);           /* volume */
    regw(0x01, 0xC0);           /* L+R output (pan bits 7/6) */
    regw(0x00, 0xA0);           /* play */

    for (i = 0; i < 40 && !playing; i++) {
        opna_advance(100000);   /* 25 ms of CPU time per step */
        playing = (opna_status_hi() & 0x20) != 0;
    }
    chk("ADPCM-B PLAYING after 0xAC/0xAD keyon", playing, 1);
    chk("port 0xAC agrees on PLAYING", (vessel_port_in(0xAC) & 0x20) != 0, 1);

    /* the sample must also END: PLAYING clears once the stop address
     * is reached, and EOS (bit 2) reads set on port 0xAC -- the exact
     * value PMD's eoscheck polls before keying the next PCM note */
    for (i = 0; i < 400 && (vessel_port_in(0xAC) & 0x20); i++)
        opna_advance(100000);
    chk("PLAYING clears after sample end",
        (vessel_port_in(0xAC) & 0x20) != 0, 0);
    chk("EOS set on port 0xAC after sample end",
        (vessel_port_in(0xAC) & 0x04) != 0, 1);

    if (fails) { printf("pcmb_ports_test: %d FAIL\n", fails); return 1; }
    printf("pcmb_ports_test: ok\n");
    return 0;
}
