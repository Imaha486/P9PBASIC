/* p88disk.h - PC-88 N88 Disk BASIC (2D) file layer over the D88
 * image the vessel already has open (P8P_DISK). Format as mapped in
 * tools/p88_extract.py: directory at C18 H1 R1-R13 (16-byte entries
 * [name 6][ext 3][attr][start cluster][0xFF x5]), FAT at C18 H1
 * R14 (one byte per cluster; 0xC1-0xDA = last cluster, low 5 bits =
 * sectors used), 1 cluster = 8 sectors numbered in (C,H,R) order.
 * Attribute bit 0x80 marks a tokenized BASIC program. */
#ifndef P88DISK_H
#define P88DISK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[16];          /* "NAME.EXT" (or "NAME"), NUL-terminated */
    unsigned char attr;
    unsigned long size;     /* bytes */
} P88FileInfo;

/* Fill up to `max` entries; returns the count, or -1 if the disk
 * cannot be read. */
int p88disk_list(P88FileInfo *out, int max);

/* Read a whole file by its "NAME.EXT" name (case-insensitive).
 * Returns the byte count, or -1 when not found / too big for cap. */
long p88disk_read(const char *name, unsigned char *dst, unsigned long cap);

#ifdef __cplusplus
}
#endif

#endif
