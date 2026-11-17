/* p88conv.h - PC-88 tokenized Disk BASIC -> BASIC source text converter
 *
 * C port of tools/p88_to_bas.py.  The output text is byte-for-byte what
 * the Python tool writes to its <out.BAS> file (latin-1 passthrough of the
 * raw program bytes, CRLF line ends).  See p88conv.c for the format notes.
 */
#ifndef P88CONV_H
#define P88CONV_H

/* Convert a tokenized PC-88 Disk BASIC program (raw bytes) to
 * BASIC source text. Returns a malloc'd buffer, or NULL on failure.
 * The text can contain embedded NUL bytes (the token stream's line
 * terminators pass through, as in the Python tool's output file),
 * so use *out_len (when non-NULL) rather than strlen. A NUL
 * terminator is still appended after the last byte for safety.
 * If report is non-NULL, *report receives a malloc'd human-readable
 * conversion report (same content the Python tool prints to
 * stderr), else NULL. */
char *p88_convert(const unsigned char *data, unsigned long len,
                  unsigned long *out_len, char **report);

#endif
