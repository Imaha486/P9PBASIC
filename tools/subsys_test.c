/* subsys_test.c - unit checks for the vessel's disk subsystem
 * write command (0x01): data written through the PUTCOM / PUTDAT
 * traps must land in the in-memory D88 image, read back through
 * the subsystem and through the file layer's raw sector path,
 * across the sector-16 -> next-track wrap - and the disk file on
 * disk must stay byte-identical (writes are session-only).
 * Usage: subsys_test <disk.d88>. Exit 0 = all checks pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vessel.h"
#include "hal.h"

static int fails;
static void chk(const char *what, long got, long want)
{
    if (got != want) { printf("FAIL %s: got %ld want %ld\n", what, got, want); fails++; }
}
static void poke(int a, int v) { hal_poke(0, a, v); }

/* 9000: CALL 37C9 / RET        (PUTCOM, A = command)
 * 9010: CALL 37D2 / RET        (PUTDAT, A = byte)
 * 9020: CALL 3847 / LD (9100),A / RET   (GETDAT -> A stored) */
static void putcom(int cmd) { vessel_call(0x9000, (unsigned)cmd, 0, 0, 0); }
static void putdat(int b)   { vessel_call(0x9010, (unsigned)b, 0, 0, 0); }
static int  getdat(void)    { vessel_call(0x9020, 0, 0, 0, 0); return hal_peek(0, 0x9100); }

static void send_params(int nsec, int t, int r)
{
    putdat(nsec); putdat(0); putdat(t); putdat(r);
}
static void read_sector_sub(int t, int r, unsigned char *dst)
{
    int i;
    putcom(0x02); send_params(1, t, r);
    putcom(0x03);
    for (i = 0; i < 256; i++) dst[i] = (unsigned char)getdat();
}
static int status(void) { putcom(0x06); return getdat(); }

static unsigned long filesum(const char *path)
{
    FILE *fp = fopen(path, "rb");
    unsigned long h = 5381; int c;
    if (!fp) return 0;
    while ((c = fgetc(fp)) != EOF) h = h * 33 + (unsigned)c;
    fclose(fp);
    return h;
}

int main(int argc, char **argv)
{
    static const unsigned char putcom_p[] = { 0xCD, 0xC9, 0x37, 0xC9 };
    static const unsigned char putdat_p[] = { 0xCD, 0xD2, 0x37, 0xC9 };
    static const unsigned char getdat_p[] = { 0xCD, 0x47, 0x38, 0x32, 0x00, 0x91, 0xC9 };
    unsigned char orig[256], back[256], via_layer[256], pat[256];
    unsigned long before, after;
    int i;
    if (argc < 2) { printf("usage: subsys_test <disk.d88>\n"); return 2; }
    before = filesum(argv[1]);
    setenv("P8P_DISK", argv[1], 1);
    vessel_init();
    for (i = 0; i < 4; i++) poke(0x9000 + i, putcom_p[i]);
    for (i = 0; i < 4; i++) poke(0x9010 + i, putdat_p[i]);
    for (i = 0; i < 7; i++) poke(0x9020 + i, getdat_p[i]);

    /* the sector exists and reads with a clean status */
    read_sector_sub(2, 1, orig);
    chk("read status", status(), 0xC0);

    /* write a pattern to track 2 sector 1 */
    for (i = 0; i < 256; i++) pat[i] = (unsigned char)(i * 7 + 3);
    putcom(0x01); send_params(1, 2, 1);
    for (i = 0; i < 256; i++) putdat(pat[i]);
    chk("write status err bit", status() & 1, 0);

    /* read back through the subsystem and the file layer */
    read_sector_sub(2, 1, back);
    chk("readback matches", memcmp(back, pat, 256), 0);
    chk("layer read ok", vessel_d88_sector(2, 1, via_layer), 1);
    chk("layer sees the write", memcmp(via_layer, pat, 256), 0);

    /* two-sector write wrapping sector 16 -> next track sector 1 */
    putcom(0x01); send_params(2, 2, 16);
    for (i = 0; i < 512; i++) putdat((i * 5 + 1) & 0xFF);
    chk("wrap write err bit", status() & 1, 0);
    chk("wrap first sector", vessel_d88_sector(2, 16, via_layer), 1);
    for (i = 0; i < 256; i++) pat[i] = (unsigned char)(i * 5 + 1);
    chk("wrap first data", memcmp(via_layer, pat, 256), 0);
    chk("wrap second sector", vessel_d88_sector(3, 1, via_layer), 1);
    for (i = 0; i < 256; i++) pat[i] = (unsigned char)((256 + i) * 5 + 1);
    chk("wrap second data", memcmp(via_layer, pat, 256), 0);

    /* the file on disk is untouched */
    after = filesum(argv[1]);
    chk("disk file unchanged", (long)(after == before), 1);

    if (fails) { printf("subsys: %d FAIL\n", fails); return 1; }
    printf("subsys ok\n");
    return 0;
}
