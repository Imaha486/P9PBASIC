/* p88disk.c - see p88disk.h. Reads go through vessel_d88_sector,
 * so the file layer and the Z80's disk subsystem share one image. */
#include <string.h>
#include <ctype.h>
#include "p88disk.h"
#include "vessel.h"

#define DIR_SECTORS 13              /* directory at C18 H1 R1-R13 */
#define FAT_SECTOR 14

static int read_sector(int c, int h, int r, unsigned char *dst)
{
    return vessel_d88_sector(c * 2 + h, r, dst);
}

/* global sector index (cluster*8 + pos) -> one 256-byte sector */
static int stream_sector(unsigned long idx, unsigned char *dst)
{
    int c = (int)(idx / 32), rem = (int)(idx % 32);
    return read_sector(c, rem / 16, rem % 16 + 1, dst);
}

static int ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Walk the directory; when `want` is non-NULL stop at that file.
 * Fills name/attr/start for one entry per call of the callback. */
struct ent { char name[16]; unsigned char attr; int start; };

static int scan_dir(struct ent *ents, int max)
{
    unsigned char sec[256];
    int n = 0, r, i;
    for (r = 1; r <= DIR_SECTORS; r++) {
        if (!read_sector(18, 1, r, sec)) return n ? n : -1;
        for (i = 0; i < 16; i++) {
            const unsigned char *e = sec + i * 16;
            int k, o = 0;
            if (e[0] == 0xFF) return n;         /* end of directory */
            if (e[0] == 0x00) continue;         /* deleted */
            if (n >= max) return n;
            for (k = 0; k < 6 && e[k] != ' '; k++) ents[n].name[o++] = (char)e[k];
            if (e[6] != ' ') {
                ents[n].name[o++] = '.';
                for (k = 6; k < 9 && e[k] != ' '; k++) ents[n].name[o++] = (char)e[k];
            }
            ents[n].name[o] = 0;
            /* The name becomes a host file name in the scratch dir
             * (materialize.c), and the image is untrusted: path
             * separators, control characters and a leading '.' could
             * name something outside it ("../x", "/a"). Such an entry
             * is skipped outright - neither listed nor readable -
             * rather than renamed: a replacement character could make
             * two entries collide ("A/B" and "A_B") and serve the
             * wrong file. Every reader of the directory goes through
             * this function, so they all agree on what exists. */
            for (k = 0; k < o; k++) {
                unsigned char ch = (unsigned char)ents[n].name[k];
                if (ch < 0x20 || ch == 0x7F || ch == '/' || ch == '\\') break;
            }
            if (k < o || ents[n].name[0] == '.') continue;
            ents[n].attr = e[9];
            ents[n].start = e[10];
            n++;
        }
    }
    return n;
}

static long file_size(const struct ent *e, const unsigned char *fat)
{
    long total = 0;
    int cur = e->start, guard = 0;
    for (;;) {
        unsigned v;
        if (cur < 0 || cur > 0xDA || guard++ > 200) return -1;
        v = fat[cur];
        if (v >= 0xC1 && v <= 0xDA) { total += (long)(v & 0x1F) * 256; break; }
        total += 2048;
        cur = v;
    }
    return total;
}

int p88disk_list(P88FileInfo *out, int max)
{
    struct ent ents[64];
    unsigned char fat[256];
    int n, i;
    if (!read_sector(18, 1, FAT_SECTOR, fat)) return -1;
    n = scan_dir(ents, 64);
    if (n < 0) return -1;
    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        long sz = file_size(&ents[i], fat);
        strcpy(out[i].name, ents[i].name);
        out[i].attr = ents[i].attr;
        out[i].size = sz < 0 ? 0 : (unsigned long)sz;
    }
    return n;
}

long p88disk_read(const char *name, unsigned char *dst, unsigned long cap)
{
    struct ent ents[64];
    unsigned char fat[256], sec[256];
    int n, i, cur, guard = 0;
    unsigned long done = 0;
    if (!read_sector(18, 1, FAT_SECTOR, fat)) return -1;
    n = scan_dir(ents, 64);
    if (n < 0) return -1;
    for (i = 0; i < n; i++) if (ci_equal(ents[i].name, name)) break;
    if (i == n) return -1;
    cur = ents[i].start;
    for (;;) {
        unsigned v;
        int j, last;
        if (cur < 0 || cur > 0xDA || guard++ > 200) return -1;
        v = fat[cur];
        last = (v >= 0xC1 && v <= 0xDA);
        {
            int nsec = last ? (v & 0x1F) : 8;
            for (j = 0; j < nsec; j++) {
                if (done + 256 > cap) return -1;
                if (!stream_sector((unsigned long)cur * 8 + (unsigned long)j, sec)) return -1;
                memcpy(dst + done, sec, 256);
                done += 256;
            }
        }
        if (last) break;
        cur = v;
    }
    /* trim a trailing ^Z-padded tail is the caller's business; the
     * FAT-derived size already excludes unused sectors of the last
     * cluster, but a file's true end inside its last sector is only
     * known to its format (tokenized BASIC ends with 0xFF fill). */
    return (long)done;
}
