/* d88fix_test.c - disk-layer boundary checks on synthetic D88 images
 * (review items C-2 / L-2). The fixtures are built here, in a scratch
 * directory, so the test needs no game disk:
 *
 *   two.d88     two bundled images. Image 0 is a small Disk BASIC
 *               volume (directory + FAT at C18 H1) holding DATA.BIN,
 *               an entry named "../X" and an entry whose FAT chain
 *               loops; image 1 carries different sector data.
 *   cut1.d88    two.d88 cut inside image 1's first track
 *   cut2.d88    two.d88 cut inside image 1's header (an "A-only" file)
 *   tiny.d88    the first 100 bytes of two.d88
 *   max.d88     two.d88 zero-padded to exactly the 2 MiB image buffer
 *   over.d88    one byte more than that
 *   broken.d88  one image with a broken track table: an offset past
 *               the file, a sector header straddling the end of the
 *               file, a track whose sector count lies, and a
 *               zero-length sector as the last thing in the file
 *
 * and run through the raw sector reader (vessel_d88_sector), the XDE
 * natives (vessel_disk_read / vessel_dram_load), the disk subsystem
 * commands (PUTCOM / PUTDAT / GETDAT traps), the Disk BASIC file
 * layer (p88disk) and the materializer. The rule under test is FAIL,
 * never substitute: a request for an image or drive the file does not
 * hold, for a missing or cut-off sector, or for more sectors than the
 * subsystem's transfer buffer holds must fail and leave the target
 * untouched - not quietly serve image 0 or a shortened transfer.
 *
 * Usage: d88fix_test [-v]   (-v keeps the vessel's stderr messages;
 * the refusals under test print some by design). Exit 0 = all pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "vessel.h"
#include "hal.h"
#include "opna.h"
#include "p88disk.h"
#include "materialize.h"

#define HDR      (0x20 + 164 * 4)   /* image header + track table */
#define SECLEN   (16 + 256)         /* sector header + data */
#define D88_MAX  0x200000L          /* the vessel's image buffer */
#define DIR_TRACK 37                /* C18 H1 */
#define LOADADDR 0x4000u            /* guest address for XDE reads */

static int fails;
static void chk(const char *what, long got, long want)
{
    if (got != want) { printf("FAIL %s: got %ld want %ld\n", what, got, want); fails++; }
}

/* ---- fixture builder ---- */

static unsigned char fb[D88_MAX + 16];
static long flen;

/* every data byte identifies its image, track and sector */
static unsigned char pat(int img, int t, int r, int i)
{
    return (unsigned char)(img * 0x55 + t * 31 + r * 7 + i * 3 + 1);
}

static void put16(long p, unsigned v)
{
    fb[p] = (unsigned char)(v & 0xFF);
    fb[p + 1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(long p, unsigned long v)
{
    put16(p, (unsigned)(v & 0xFFFF));
    put16(p + 2, (unsigned)((v >> 16) & 0xFFFF));
}

static long img_begin(const char *name)
{
    long base = flen;
    memset(fb + base, 0, HDR);
    strncpy((char *)fb + base, name, 16);
    flen += HDR;
    return base;
}

static void img_end(long base)
{
    put32(base + 0x1C, (unsigned long)(flen - base));
}

/* the next sector appended starts logical track t of the image */
static void track_begin(long base, int t)
{
    put32(base + 0x20 + t * 4L, (unsigned long)(flen - base));
}

/* append one sector: `count` is the header's sectors-per-track
 * field, n the size code, dsize the data bytes that follow */
static void add_sector(int img, int t, int r, int count, int n, int dsize)
{
    long p = flen;
    int i;
    memset(fb + p, 0, 16);
    fb[p] = (unsigned char)(t >> 1);
    fb[p + 1] = (unsigned char)(t & 1);
    fb[p + 2] = (unsigned char)r;
    fb[p + 3] = (unsigned char)n;
    put16(p + 4, (unsigned)count);
    put16(p + 0x0E, (unsigned)dsize);
    for (i = 0; i < dsize; i++) fb[p + 16 + i] = pat(img, t, r, i);
    flen = p + 16 + dsize;
}

static void add_track16(long base, int img, int t)
{
    int r;
    track_begin(base, t);
    for (r = 1; r <= 16; r++) add_sector(img, t, r, 16, 1, 256);
}

/* data of sector r in a track written by add_track16 */
static unsigned char *sec_data(long base, int t, int r)
{
    long off = (long)fb[base + 0x20 + t * 4L]
             | ((long)fb[base + 0x21 + t * 4L] << 8)
             | ((long)fb[base + 0x22 + t * 4L] << 16);
    return fb + base + off + (long)(r - 1) * SECLEN + 16;
}

static void dir_entry(unsigned char *e, const char *name, const char *ext,
                      int attr, int start)
{
    size_t k;
    memset(e, ' ', 9);
    memset(e + 9, 0xFF, 7);
    for (k = 0; k < 6 && name[k]; k++) e[k] = (unsigned char)name[k];
    for (k = 0; k < 3 && ext[k]; k++) e[6 + k] = (unsigned char)ext[k];
    e[9] = (unsigned char)attr;
    e[10] = (unsigned char)start;
}

/* two.d88; returns the file offset of image 1 */
static long build_two(void)
{
    long a, b;
    unsigned char *dir, *fat;
    int t;
    flen = 0;
    a = img_begin("FIXA");
    for (t = 0; t < 8; t++) add_track16(a, 0, t);
    add_track16(a, 0, DIR_TRACK);
    /* directory (R1) and FAT (R14). Clusters are 8 sectors in
     * (C,H,R) order: cluster 0 = track 0 R1-8, 1 = track 0 R9-16,
     * 2 = track 1 R1-8, 5 = track 2 R9-16. */
    dir = sec_data(a, DIR_TRACK, 1);
    memset(dir, 0xFF, 256);
    dir_entry(dir, "DATA", "BIN", 0, 0);        /* clusters 0 -> 1 */
    dir_entry(dir + 16, "../X", "", 0, 2);      /* escapes the scratch dir */
    dir_entry(dir + 32, "LOOP", "", 0, 5);      /* FAT chain loops */
    fat = sec_data(a, DIR_TRACK, 14);
    memset(fat, 0xFF, 256);
    fat[0] = 1;
    fat[1] = 0xC3;                              /* last, 3 sectors used */
    fat[2] = 0xC1;
    fat[5] = 5;
    img_end(a);
    b = img_begin("FIXB");
    for (t = 0; t < 4; t++) add_track16(b, 1, t);
    img_end(b);
    return b;
}

static void build_broken(void)
{
    long a;
    flen = 0;
    a = img_begin("BRK");
    add_track16(a, 0, 0);
    put32(a + 0x20 + 1 * 4L, 0x00FFFFF0UL);     /* track 1: past the file */
    /* track 3: the header claims 1000 sectors, one follows */
    track_begin(a, 3);
    add_sector(0, 3, 1, 1000, 1, 256);
    /* track 4: a zero-length sector, the last bytes of the file */
    track_begin(a, 4);
    add_sector(0, 4, 1, 1, 1, 0);
    /* track 2: its sector header would straddle the end of the file */
    put32(a + 0x20 + 2 * 4L, (unsigned long)(flen - a - 8));
    img_end(a);
}

/* ---- scratch directory ---- */

static char tdir[200];

static const char *tpath(const char *name)
{
    static char p[320];
    snprintf(p, sizeof(p), "%s/%s", tdir, name);
    return p;
}

static void save(const char *name, long len)
{
    FILE *fp = fopen(tpath(name), "wb");
    if (!fp || fwrite(fb, 1, (size_t)len, fp) != (size_t)len) {
        printf("FAIL cannot write fixture %s\n", name);
        fails++;
    }
    if (fp) fclose(fp);
}

static void rm_tree(const char *path, int depth)
{
    DIR *d = opendir(path);
    struct dirent *de;
    if (d) {
        while ((de = readdir(d)) != 0) {
            char p[600];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            snprintf(p, sizeof(p), "%s/%s", path, de->d_name);
            if (depth < 2) rm_tree(p, depth + 1);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(path);
}

static int dir_count(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    int n = 0;
    if (!d) return -1;
    while ((de = readdir(d)) != 0)
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) n++;
    closedir(d);
    return n;
}

/* make `name` the disk the vessel reads from now on */
static void bind_disk(const char *name)
{
    setenv("P8P_DISK", tpath(name), 1);
    vessel_forget_disk();
}

/* ---- guest-side helpers ---- */

static void fill_guest(unsigned addr, int len, int v)
{
    int i;
    for (i = 0; i < len; i++) hal_poke(0, addr + (unsigned)i, v);
}

/* bytes at addr that differ from sector (img, t, r) */
static long guest_diff(unsigned addr, int img, int t, int r)
{
    long d = 0;
    int i;
    for (i = 0; i < 256; i++)
        if (hal_peek(0, addr + (unsigned)i) != pat(img, t, r, i)) d++;
    return d;
}

/* bytes at addr that are not v */
static long guest_not(unsigned addr, int len, int v)
{
    long d = 0;
    int i;
    for (i = 0; i < len; i++)
        if (hal_peek(0, addr + (unsigned)i) != v) d++;
    return d;
}

static long buf_diff(const unsigned char *b, int img, int t, int r)
{
    long d = 0;
    int i;
    for (i = 0; i < 256; i++) if (b[i] != pat(img, t, r, i)) d++;
    return d;
}

/* subsystem access through the ROM entry traps, as tools/subsys_test.c:
 * 9000: CALL 37C9 / RET        (PUTCOM, A = command)
 * 9010: CALL 37D2 / RET        (PUTDAT, A = byte)
 * 9020: CALL 3847 / LD (9100),A / RET   (GETDAT -> A stored) */
static void putcom(int cmd) { vessel_call(0x9000, (unsigned)cmd, 0, 0, 0); }
static void putdat(int b)   { vessel_call(0x9010, (unsigned)b, 0, 0, 0); }
static int  getdat(void)    { vessel_call(0x9020, 0, 0, 0, 0); return hal_peek(0, 0x9100); }
static int  status(void)    { putcom(0x06); return getdat(); }

/* read command; returns the status byte (C0 = data valid, no error) */
static int sub_read(int nsec, int drive, int t, int r)
{
    putcom(0x02);
    putdat(nsec); putdat(drive); putdat(t); putdat(r);
    return status();
}

/* write command with nsec sectors of byte v; returns the status byte */
static int sub_write(int nsec, int drive, int t, int r, int v)
{
    long i;
    putcom(0x01);
    putdat(nsec); putdat(drive); putdat(t); putdat(r);
    for (i = 0; i < (long)nsec * 256; i++) putdat(v);
    return status();
}

/* send-data command: fetch the first 256 bytes of the read buffer */
static void sub_fetch(unsigned char *dst)
{
    int i;
    putcom(0x03);
    for (i = 0; i < 256; i++) dst[i] = (unsigned char)getdat();
}

/* ---- the checks ---- */

static void check_two(void)
{
    static unsigned char big[1 << 16];
    unsigned char sec[256];
    P88FileInfo files[8];
    char boot[32];
    char *dir;
    long n, k, d;

    bind_disk("two.d88");

    /* raw reader: present, absent track, absent sector */
    chk("two: sector reads", vessel_d88_sector(2, 1, sec), 1);
    chk("two: sector data", buf_diff(sec, 0, 2, 1), 0);
    chk("two: track without table entry fails", vessel_d88_sector(8, 1, sec), 0);
    chk("two: sector id not on the track fails", vessel_d88_sector(2, 17, sec), 0);
    chk("two: track number out of range fails", vessel_d88_sector(164, 1, sec), 0);

    /* XDE service 0: the disk argument selects the image */
    fill_guest(LOADADDR, 512, 0xEE);
    chk("two: image 0 read count", vessel_disk_read(0, 2, 16, 2, LOADADDR), 2);
    chk("two: image 0 data", guest_diff(LOADADDR, 0, 2, 16), 0);
    chk("two: image 0 wraps to the next track", guest_diff(LOADADDR + 256, 0, 3, 1), 0);
    chk("two: image 1 read count", vessel_disk_read(1, 2, 1, 1, LOADADDR), 1);
    chk("two: image 1 data is image 1's", guest_diff(LOADADDR, 1, 2, 1), 0);
    /* L-2: an image the file does not hold FAILS */
    fill_guest(LOADADDR, 512, 0xEE);
    chk("two: image 2 read fails", vessel_disk_read(2, 2, 1, 2, LOADADDR), 0);
    chk("two: image -1 read fails", vessel_disk_read(-1, 2, 1, 2, LOADADDR), 0);
    chk("two: failed read leaves memory alone", guest_not(LOADADDR, 512, 0xEE), 0);
    /* a read that runs off the image stops at the last sector */
    chk("two: read stops at the missing track", vessel_disk_read(1, 3, 15, 4, LOADADDR), 2);

    /* XDE service 1 */
    opna_reset();
    chk("two: DRAM load from image 1", vessel_dram_load(1, 2, 1), 16);
    for (d = 0, k = 0; k < 256; k++)
        if (opna_adpcm_b_read((unsigned long)k) != pat(1, 2, 1, (int)k)) d++;
    chk("two: DRAM holds image 1's sector", d, 0);
    chk("two: DRAM load from image 2 fails", vessel_dram_load(2, 2, 1), 0);
    chk("two: failed DRAM load leaves the DRAM alone",
        opna_adpcm_b_read(0), pat(1, 2, 1, 0));
    chk("two: DRAM load stops at the missing track", vessel_dram_load(0, 6, 4), 32);

    /* disk subsystem: drive n = image n, a drive without an image and
     * an oversized request fail, in both directions */
    chk("two: subsys read drive 0", sub_read(1, 0, 2, 1), 0xC0);
    sub_fetch(sec);
    chk("two: subsys drive 0 data", buf_diff(sec, 0, 2, 1), 0);
    chk("two: subsys read drive 1", sub_read(1, 1, 2, 1), 0xC0);
    sub_fetch(sec);
    chk("two: subsys drive 1 data is image 1's", buf_diff(sec, 1, 2, 1), 0);
    chk("two: subsys read drive 2 fails", sub_read(1, 2, 2, 1), 0x81);
    putcom(0x03);
    chk("two: failed read sends no data", getdat(), 0xFF);
    /* a read that fails part-way sends nothing either: image 0's
     * track 7 is its last, so two sectors from T7 R16 are one
     * readable sector and one off the fixture (track 8, no table
     * entry). The readable first sector must not reach the guest. */
    chk("two: subsys read up to the boundary", sub_read(1, 0, 7, 16), 0xC0);
    sub_fetch(sec);
    chk("two: subsys boundary sector data", buf_diff(sec, 0, 7, 16), 0);
    chk("two: subsys read across the boundary fails", sub_read(2, 0, 7, 16), 0x81);
    chk("two: the error is reported once", status(), 0x80);
    putcom(0x03);
    chk("two: partial read sends no data", getdat(), 0xFF);
    putcom(0x12);
    chk("two: partial read sends no data (12H)", getdat(), 0xFF);
    /* a multi-sector read that succeeds is sent whole */
    chk("two: subsys read of 2 sectors", sub_read(2, 0, 6, 16), 0xC0);
    putcom(0x03);
    for (k = 0; k < 256; k++) sec[k] = (unsigned char)getdat();
    chk("two: 2-sector read, first sector", buf_diff(sec, 0, 6, 16), 0);
    for (k = 0; k < 256; k++) sec[k] = (unsigned char)getdat();
    chk("two: 2-sector read, second sector", buf_diff(sec, 0, 7, 1), 0);
    chk("two: 2-sector read sends 512 bytes", getdat(), 0xFF);
    chk("two: subsys read of 64 sectors", sub_read(64, 0, 2, 1), 0xC0);
    chk("two: subsys read of 65 sectors fails", sub_read(65, 0, 2, 1), 0x81);
    chk("two: subsys write of 65 sectors fails", sub_write(65, 0, 2, 1, 0x5A) & 1, 1);
    chk("two: refused write wrote nothing", vessel_d88_sector(2, 1, sec), 1);
    chk("two: refused write data", buf_diff(sec, 0, 2, 1), 0);
    chk("two: subsys write to drive 2 fails", sub_write(1, 2, 3, 1, 0x5A) & 1, 1);
    chk("two: subsys write to drive 1", sub_write(1, 1, 3, 1, 0x5A) & 1, 0);
    chk("two: drive 1 write lands in image 1", vessel_disk_read(1, 3, 1, 1, LOADADDR), 1);
    chk("two: drive 1 write data", guest_not(LOADADDR, 256, 0x5A), 0);
    chk("two: image 0 keeps its sector", vessel_d88_sector(3, 1, sec), 1);
    chk("two: image 0 sector data", buf_diff(sec, 0, 3, 1), 0);

    /* Disk BASIC file layer (image 0) */
    n = p88disk_list(files, 8);
    chk("two: directory entries", n, 2);        /* "../X" is not one */
    if (n == 2) {
        chk("two: first entry is DATA.BIN", strcmp(files[0].name, "DATA.BIN"), 0);
        chk("two: DATA.BIN size", (long)files[0].size, 11 * 256L);
        chk("two: second entry is LOOP", strcmp(files[1].name, "LOOP"), 0);
        chk("two: looping chain has no size", (long)files[1].size, 0);
    }
    n = p88disk_read("data.bin", big, sizeof(big));
    chk("two: DATA.BIN reads", n, 11 * 256L);
    for (d = 0, k = 0; n == 11 * 256L && k < 11; k++)
        d += buf_diff(big + k * 256, 0, 0, (int)k + 1);
    chk("two: DATA.BIN data", d, 0);
    chk("two: DATA.BIN over the caller's cap fails", p88disk_read("DATA.BIN", big, 1024), -1);
    chk("two: ../X is not readable", p88disk_read("../X", big, sizeof(big)), -1);
    chk("two: looping chain fails", p88disk_read("LOOP", big, sizeof(big)), -1);
    chk("two: absent file fails", p88disk_read("NOPE", big, sizeof(big)), -1);

    /* materializer: DATA.BIN only, nothing outside the scratch dir */
    dir = p8_materialize(0, 0);
    chk("two: materialized", dir != 0, 1);
    if (dir) {
        char p[600];
        struct stat st;
        memset(&st, 0, sizeof(st));
        snprintf(p, sizeof(p), "%s/DATA.BIN", dir);
        chk("two: DATA.BIN materialized", stat(p, &st), 0);
        chk("two: materialized size", (long)st.st_size, 11 * 256L);
        chk("two: scratch dir holds one file", dir_count(dir), 1);
        chk("two: no file escaped the scratch dir", access(tpath("X"), F_OK) != 0, 1);
        free(dir);
    }
    /* a caller that needs a boot program gets a failure: none here */
    dir = p8_materialize(boot, sizeof(boot));
    chk("two: no boot program, no boot", dir == 0, 1);
    free(dir);
}

static void check_cuts(void)
{
    unsigned char sec[256];
    P88FileInfo files[8];
    char *dir;

    /* cut inside image 1's first track: 5 whole sectors survive */
    bind_disk("cut1.d88");
    fill_guest(LOADADDR, 16 * 256, 0xEE);
    chk("cut1: read stops at the cut", vessel_disk_read(1, 0, 1, 16, LOADADDR), 5);
    chk("cut1: last whole sector", guest_diff(LOADADDR + 4 * 256, 1, 0, 5), 0);
    chk("cut1: nothing past the cut", guest_not(LOADADDR + 5 * 256, 11 * 256, 0xEE), 0);
    chk("cut1: image 0 intact", vessel_d88_sector(7, 16, sec), 1);

    /* cut inside image 1's header: the file holds image 0 only, so an
     * image 1 request must fail instead of playing image 0's bytes */
    bind_disk("cut2.d88");
    fill_guest(LOADADDR, 256, 0xEE);
    chk("cut2: image 1 read fails", vessel_disk_read(1, 0, 1, 1, LOADADDR), 0);
    chk("cut2: memory untouched", guest_not(LOADADDR, 256, 0xEE), 0);
    chk("cut2: subsys drive 1 fails", sub_read(1, 1, 0, 1), 0x81);
    chk("cut2: DRAM load from image 1 fails", vessel_dram_load(1, 0, 1), 0);
    chk("cut2: image 0 still reads", vessel_disk_read(0, 0, 1, 1, LOADADDR), 1);
    chk("cut2: image 0 data", guest_diff(LOADADDR, 0, 0, 1), 0);

    /* shorter than one header: no disk at all */
    bind_disk("tiny.d88");
    chk("tiny: sector read fails", vessel_d88_sector(0, 1, sec), 0);
    chk("tiny: XDE read fails", vessel_disk_read(0, 0, 1, 1, LOADADDR), 0);
    chk("tiny: directory unreadable", p88disk_list(files, 8), -1);
    dir = p8_materialize(0, 0);
    chk("tiny: materialize fails", dir == 0, 1);
    free(dir);

    /* exactly the image buffer loads; one byte more is refused whole */
    bind_disk("max.d88");
    chk("max: loads", vessel_d88_sector(DIR_TRACK, 14, sec), 1);
    bind_disk("over.d88");
    chk("over: refused", vessel_d88_sector(0, 1, sec), 0);
    chk("over: XDE read fails", vessel_disk_read(0, 0, 1, 1, LOADADDR), 0);
    chk("over: directory unreadable", p88disk_list(files, 8), -1);

    /* no disk file at all */
    bind_disk("absent.d88");
    chk("absent: sector read fails", vessel_d88_sector(0, 1, sec), 0);
    chk("absent: subsys read fails", sub_read(1, 0, 0, 1), 0x81);
}

static void check_broken(void)
{
    unsigned char sec[256];
    P88FileInfo files[8];

    bind_disk("broken.d88");
    chk("broken: good track reads", vessel_d88_sector(0, 16, sec), 1);
    chk("broken: good track data", buf_diff(sec, 0, 0, 16), 0);
    chk("broken: offset past the file fails", vessel_d88_sector(1, 1, sec), 0);
    chk("broken: header across the end fails", vessel_d88_sector(2, 1, sec), 0);
    chk("broken: lying count, present sector", vessel_d88_sector(3, 1, sec), 1);
    chk("broken: lying count, sector data", buf_diff(sec, 0, 3, 1), 0);
    chk("broken: lying count, absent sector fails", vessel_d88_sector(3, 9, sec), 0);
    chk("broken: zero-length sector fails", vessel_d88_sector(4, 1, sec), 0);
    chk("broken: read across the bad track stops", vessel_disk_read(0, 0, 16, 2, LOADADDR), 1);
    chk("broken: subsys read of the bad track fails", sub_read(1, 0, 1, 1), 0x81);
    chk("broken: no directory track", p88disk_list(files, 8), -1);
}

int main(int argc, char **argv)
{
    static const unsigned char putcom_p[] = { 0xCD, 0xC9, 0x37, 0xC9 };
    static const unsigned char putdat_p[] = { 0xCD, 0xD2, 0x37, 0xC9 };
    static const unsigned char getdat_p[] = { 0xCD, 0x47, 0x38, 0x32, 0x00, 0x91, 0xC9 };
    const char *tmp = getenv("TMPDIR");
    long img1, len;
    int i;

    if (!(argc > 1 && !strcmp(argv[1], "-v")) && !freopen("/dev/null", "w", stderr))
        return 2;
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(tdir, sizeof(tdir), "%s/d88fixXXXXXX", tmp);
    if (!mkdtemp(tdir)) { printf("cannot create a scratch directory\n"); return 2; }
    /* the materializer's own scratch directories go inside ours */
    setenv("TMPDIR", tdir, 1);

    img1 = build_two();
    len = flen;
    save("two.d88", len);
    save("cut1.d88", img1 + HDR + 5 * SECLEN + 100);
    save("cut2.d88", img1 + 100);
    save("tiny.d88", 100);
    save("max.d88", D88_MAX);           /* fb is zero past the images */
    save("over.d88", D88_MAX + 1);
    build_broken();
    save("broken.d88", flen);

    bind_disk("two.d88");
    vessel_init();
    for (i = 0; i < 4; i++) hal_poke(0, 0x9000 + i, putcom_p[i]);
    for (i = 0; i < 4; i++) hal_poke(0, 0x9010 + i, putdat_p[i]);
    for (i = 0; i < 7; i++) hal_poke(0, 0x9020 + i, getdat_p[i]);

    if (!fails) {
        check_two();
        check_cuts();
        check_broken();
    }

    rm_tree(tdir, 0);
    if (fails) { printf("d88fix: %d FAIL\n", fails); return 1; }
    printf("d88fix ok\n");
    return 0;
}
