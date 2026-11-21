/* materialize.h - write a D88's files into a scratch directory so
 * the interpreter's plain-file layer can work on them. Shared by
 * the embedded engine (src/engine.c) and the host CLI (src/main.c,
 * when built with P8_HOST_MATERIALIZE). */
#ifndef P8_MATERIALIZE_H
#define P8_MATERIALIZE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The disk image is the one the vessel's D88 layer reads (the
 * P8P_DISK environment variable) - the same image the Z80 disk
 * subsystem sees. Tokenized BASIC programs (attr 0x80) become
 * <base>.BAS text (see src/engine.c's history for the MENU.ALP
 * naming rule); other files are written verbatim. The scratch
 * directory is created under TMPDIR and NOT removed: guest file
 * writes land there. Returns the scratch path (malloc'd) or NULL.
 * When bootfile is non-NULL it receives the boot program's file
 * name (MENU preferred), or "" when the disk holds no program. */
char *p8_materialize(char *bootfile, size_t bootfile_cap);

#ifdef __cplusplus
}
#endif

#endif
