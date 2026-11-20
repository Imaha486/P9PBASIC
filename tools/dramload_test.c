/* dramload_test.c - unit check for the FFDE service 1 equivalent
 * (vessel_dram_load): the PCM data block must land in the OPNA's
 * ADPCM-B sample DRAM (what PMD's PCM part plays through the
 * bank-1 registers), byte-identical to the disk's sectors, and the
 * ADPCM-A memory (the chip's rhythm ROM, not a load target) must
 * stay all zero. The expected bytes are parsed from the D88 file
 * here, independently of the vessel's reader.
 * Usage: dramload_test <disk.d88>. Exit 0 = all checks pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vessel.h"
#include "opna.h"

#define LTRACK 2            /* first logical track loaded */
#define TRACKS 2            /* 2 tracks = 32 sectors = 8 KiB */
#define NBYTES (TRACKS * 16 * 256L)
#define B_SIZE (256L * 1024)
#define A_SIZE (1024L * 1024)

static int fails;
static void chk(const char *what, long got, long want)
{
    if (got != want) { printf("FAIL %s: got %ld want %ld\n", what, got, want); fails++; }
}

static unsigned char *img;
static long img_size;
static unsigned long u16(long p) { return img[p] | ((unsigned long)img[p + 1] << 8); }
static unsigned long u32(long p) { return u16(p) | (u16(p + 2) << 16); }

/* sector r of logical track t of the file's first disk image */
static int file_sector(int t, int r, unsigned char *dst)
{
    long pos;
    int n, count;
    if (0x20 + t * 4L + 4 > img_size) return 0;
    pos = (long)u32(0x20 + t * 4L);
    if (pos == 0 || pos + 16 > img_size) return 0;
    count = (int)u16(pos + 4);
    for (n = 0; n < count && pos + 16 <= img_size; n++) {
        long dsize = (long)u16(pos + 0x0E);
        if (img[pos + 2] == r) {
            if (dsize != 256 || pos + 16 + 256 > img_size) return 0;
            memcpy(dst, img + pos + 16, 256);
            return 1;
        }
        pos += 16 + dsize;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static unsigned char want[NBYTES];
    FILE *fp;
    long i, diff = 0, nonzero = 0, tail = 0, a_nonzero = 0;
    int t, r;
    if (argc < 2) { printf("usage: dramload_test <disk.d88>\n"); return 2; }
    fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(fp, 0, SEEK_END); img_size = ftell(fp); fseek(fp, 0, SEEK_SET);
    img = malloc((size_t)img_size);
    if (!img || fread(img, 1, (size_t)img_size, fp) != (size_t)img_size) {
        printf("cannot read %s\n", argv[1]); return 2;
    }
    fclose(fp);
    for (t = 0; t < TRACKS; t++)
        for (r = 1; r <= 16; r++)
            chk("disk sector present",
                file_sector(LTRACK + t, r, want + (t * 16 + r - 1) * 256L), 1);
    for (i = 0; i < NBYTES; i++) if (want[i]) nonzero++;
    /* an all-zero block could not tell the two memories apart */
    chk("disk block has data", nonzero > 0, 1);

    setenv("P8P_DISK", argv[1], 1);
    vessel_init();
    opna_reset();

    chk("sectors loaded", vessel_dram_load(0, LTRACK, TRACKS), TRACKS * 16);

    for (i = 0; i < NBYTES; i++)
        if (opna_adpcm_b_read((unsigned long)i) != want[i]) diff++;
    chk("ADPCM-B DRAM matches the disk sectors", diff, 0);
    for (i = NBYTES; i < B_SIZE; i++)
        if (opna_adpcm_b_read((unsigned long)i)) tail++;
    chk("ADPCM-B DRAM beyond the block untouched", tail, 0);
    for (i = 0; i < A_SIZE; i++)
        if (opna_adpcm_a_read((unsigned long)i)) a_nonzero++;
    chk("ADPCM-A memory stays zero", a_nonzero, 0);

    if (fails) { printf("dramload: %d FAIL\n", fails); return 1; }
    printf("dramload ok\n");
    return 0;
}
