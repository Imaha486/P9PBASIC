/* p88conv.c - PC-88 tokenized Disk BASIC -> BASIC source text converter
 *
 * C port of tools/p88_to_bas.py (token tables shared with
 * tools/p88_detok.py; format notes in docs/TOKEN-FORMAT.md).  The Python
 * tool is the spec: the text produced here must match the file it writes
 * byte for byte, so several of its quirks are reproduced on purpose and
 * marked "quirk" below.  Don't "fix" them here without fixing the Python
 * side too, or the two drift apart.
 *
 * Adaptations made while converting (P8PBASIC lacks these PC-88 features):
 *   - Labels: a `*NAME` definition at line start is dropped (the line keeps
 *     its number and any following statements).  References after
 *     GOTO/GOSUB/RESTORE/THEN/ELSE become the defining line's number;
 *     THEN/ELSE gain an explicit GOTO.
 *   - STOP traps: `STOP ON` and `ON STOP GOSUB *x` are dropped.
 *   - Driver extension statements FFDD/FFDE/FFDF become XDD/XDE/XDF.
 *
 * Text model: the Python tool builds a str with chr(byte) per raw byte and
 * writes it encoded as latin-1, i.e. raw bytes (Shift_JIS in strings and
 * comments) pass straight through.  We just copy bytes.
 *
 * Build the standalone driver with -DP88CONV_TEST:
 *   p88conv <tokenized-file>   (text to stdout, report to stderr)
 */
#include "p88conv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* growable byte buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct { char *p; size_t n, cap; int oom; } Buf;

static void buf_putn(Buf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *np;
        while (b->n + n + 1 > cap) cap *= 2;
        np = (char *)realloc(b->p, cap);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
static void buf_putc(Buf *b, int c) { char ch = (char)c; buf_putn(b, &ch, 1); }

static void buf_putl(Buf *b, long v)
{
    char tmp[32];
    sprintf(tmp, "%ld", v);
    buf_puts(b, tmp);
}

/* ------------------------------------------------------------------ */
/* token tables (p88_detok.py TOK / FUNC / OPS)                       */
/* ------------------------------------------------------------------ */

/* One-byte statement/keyword tokens.  GW-BASIC order for 81-B0, PC-88
 * extensions in C0-FE identified from MENU.ALP / menu.  0x8F alone is the
 * apostrophe; 8F E9 is the comment form and is handled separately. */
static const char *tok_name(int b)
{
    static const char *const lo[] = {          /* 0x81 .. 0xB0 */
        "END","FOR","NEXT","DATA","INPUT","DIM","READ",
        "LET","GOTO","RUN","IF","RESTORE","GOSUB","RETURN",
        "'","STOP","PRINT","CLEAR","LIST","NEW","ON",
        "WAIT","DEF","POKE","CONT","OUT","LPRINT","LLIST",
        "CONSOLE","WIDTH","ELSE","TRON","TROFF","SWAP",
        "ERASE","ERROR","RESUME","DELETE","AUTO","RENUM",
        "DEFSTR","DEFINT","DEFSNG","DEFDBL",
        0 /* 0xAD unassigned */, "LINE","WHILE","WEND"
    };
    if (b >= 0x81 && b <= 0xB0) return lo[b - 0x81];
    switch (b) {
    case 0xCB: return "COLOR";
    case 0xCE: return "CLS";
    case 0xD3: return "SCREEN";
    case 0xD4: return "BLOAD";
    case 0xD6: return "LOCATE";
    case 0xDC: return "TO";
    case 0xDD: return "THEN";
    }
    return 0;
}

/* FF xx function tokens.  The converter commits to INT for FF85 and
 * RIGHT$ for FF82 (the detokenizer still shows them with a '?'). */
static const char *func_name(int t)
{
    switch (t) {
    case 0x82: return "RIGHT$";
    case 0x85: return "INT";
    case 0x90: return "INP";
    case 0x92: return "LEN";
    case 0x93: return "STR$";
    case 0x94: return "VAL";
    case 0x97: return "PEEK";
    case 0x9A: return "HEX$";
    }
    return 0;
}

/* FF xx statement tokens supplied by the NEOMSC driver; their real names
 * are unknown, so the interpreter accepts them as XDD/XDE/XDF. */
static const char *ext_name(int t)
{
    switch (t) {
    case 0xDD: return "XDD";
    case 0xDE: return "XDE";
    case 0xDF: return "XDF";
    }
    return 0;
}

/* Operators F0-FE.  F5 doubles as the label marker in GOTO position. */
static const char *op_name(int b)
{
    static const char *const ops[] = {
        ">","=","<","+","-","*","/","^",
        "AND","OR","XOR","EQV","IMP","MOD","\\"
    };
    if (b >= 0xF0 && b <= 0xFE) return ops[b - 0xF0];
    return 0;
}

static int is_alnum_ascii(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

/* ------------------------------------------------------------------ */
/* numbers                                                            */
/* ------------------------------------------------------------------ */

/* Format a double exactly as Python's repr() does: the shortest digit
 * string that round-trips, fixed notation for 1e-4 <= |x| < 1e16 (with a
 * trailing ".0" when integral), otherwise d.ddde[+-]XX.  The values here
 * are always finite and non-zero (MBF exponent 0 is caught earlier). */
static void put_pyrepr(Buf *b, double x)
{
    char tmp[64], digits[32];
    int prec, nd = 0, exp10, decpt, k;
    const char *s;

    for (prec = 0; prec < 17; prec++) {        /* 1..17 significant digits */
        snprintf(tmp, sizeof tmp, "%.*e", prec, x);
        if (strtod(tmp, 0) == x) break;
    }
    s = tmp;
    if (*s == '-') { buf_putc(b, '-'); s++; }
    for (; *s && *s != 'e'; s++)
        if (*s >= '0' && *s <= '9') digits[nd++] = *s;
    exp10 = atoi(s + 1);
    while (nd > 1 && digits[nd - 1] == '0') nd--;   /* %.e may pad zeros */
    digits[nd] = 0;
    decpt = exp10 + 1;          /* digits are 0.DDDD * 10^decpt */

    if (decpt > -4 && decpt <= 16) {
        if (decpt <= 0) {
            buf_puts(b, "0.");
            for (k = 0; k < -decpt; k++) buf_putc(b, '0');
            buf_puts(b, digits);
        } else if (decpt >= nd) {
            buf_puts(b, digits);
            for (k = nd; k < decpt; k++) buf_putc(b, '0');
            buf_puts(b, ".0");
        } else {
            buf_putn(b, digits, (size_t)decpt);
            buf_putc(b, '.');
            buf_puts(b, digits + decpt);
        }
    } else {
        buf_putc(b, digits[0]);
        if (nd > 1) { buf_putc(b, '.'); buf_puts(b, digits + 1); }
        snprintf(tmp, sizeof tmp, "e%c%02d", exp10 < 0 ? '-' : '+',
                 exp10 < 0 ? -exp10 : exp10);
        buf_puts(b, tmp);
    }
}

static double scale2(double v, int e);

/* MBF single: [m0][m1][m2][exp].  Quirk (inherited from p88_detok.py):
 * the sign bit (bit 7 of m2) is not stripped and not applied; the hidden
 * 1 is OR'd in at bit 31, leaving bits 24-30 zero.  The resulting value is
 * bits * 2^(exp-151), which is exact in a double.  Short slice -> "?". */
static void put_mbf_single(Buf *b, const unsigned char *p, long avail)
{
    double bits;
    if (avail < 4) { buf_putc(b, '?'); return; }
    if (p[3] == 0) { buf_putc(b, '0'); return; }
    bits = (double)((unsigned long)p[0] | ((unsigned long)p[1] << 8) |
                    ((unsigned long)p[2] << 16) | 0x80000000UL);
    put_pyrepr(b, scale2(bits, (int)p[3] - 127 - 24));
}

/* MBF double: [m0..m5][m6: sign|m6][exp].  56-bit mantissa with hidden
 * bit 55.  Python converts the int to float first (round-half-even to 53
 * bits), which the uint64 -> double conversion here also does. */
static void put_mbf_double(Buf *b, const unsigned char *p, long avail)
{
    unsigned long long frac;
    double v;
    int k;
    if (avail < 8) { buf_putc(b, '?'); return; }
    if (p[7] == 0) { buf_putc(b, '0'); return; }
    frac = (unsigned long long)(p[6] & 0x7F) << 48;
    for (k = 5; k >= 0; k--) frac |= (unsigned long long)p[k] << (8 * k);
    frac |= 1ULL << 55;
    v = (double)frac;
    if (p[6] & 0x80) v = -v;
    put_pyrepr(b, scale2(v, (int)p[7] - 128 - 55));
}

/* v * 2^e without libm (the acceptance build links no -lm).  Each step is
 * an exact power-of-two scaling; for MBF ranges (|e| <= 183, mantissa
 * >= 2^31) the value stays well inside the normal double range. */
static double scale2(double v, int e)
{
    for (; e > 0; e--) v *= 2.0;
    for (; e < 0; e++) v *= 0.5;
    return v;
}

static unsigned rd_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }

/* ------------------------------------------------------------------ */
/* line framing                                                       */
/* ------------------------------------------------------------------ */

typedef struct { long no; long off, len; } Line;

/* Line = [link u16][lineno u16][body][00].  The link field is supposed to
 * be the file offset of the next line but drifts by 1 on some lines in the
 * real files, so (as in p88_detok.parse_lines) we probe link, link-1,
 * link+1, link-2, link+2 for a header whose line number is a little larger
 * and whose own link points forward.  When no candidate fits, this is the
 * last line: its body runs to the first 00 *inclusive* (quirk: the 00 is
 * kept, and later shows up as "<00>").  Returns the line count, or -1 on
 * allocation failure. */
static long parse_lines(const unsigned char *d, long n, Line **out)
{
    static const int delta[5] = { 0, -1, 1, -2, 2 };
    Line *v = 0;
    long cnt = 0, cap = 0, p = 0;

    while (p + 4 <= n) {
        long link = (long)rd_u16(d + p), no = (long)rd_u16(d + p + 2);
        long nxt = -1, k, off, blen;
        if (link == 0 || no == 0) break;
        for (k = 0; k < 5; k++) {
            long cand = link + delta[k], l2, n2;
            if (cand <= p + 4 || cand + 4 > n) continue;
            l2 = (long)rd_u16(d + cand);
            n2 = (long)rd_u16(d + cand + 2);
            if (no < n2 && n2 < no + 2000 && l2 > cand) { nxt = cand; break; }
        }
        if (cnt == cap) {
            Line *nv;
            cap = cap ? cap * 2 : 64;
            nv = (Line *)realloc(v, (size_t)cap * sizeof *v);
            if (!nv) { free(v); return -1; }
            v = nv;
        }
        off = p + 4;
        if (nxt < 0) {
            const unsigned char *z = (const unsigned char *)
                memchr(d + off, 0, (size_t)(n - off));
            long end = z ? (long)(z - d) + 1 : n;
            v[cnt].no = no; v[cnt].off = off; v[cnt].len = end - off;
            cnt++;
            break;
        }
        blen = nxt - off;
        if (blen > 0 && d[off + blen - 1] == 0) blen--;
        v[cnt].no = no; v[cnt].off = off; v[cnt].len = blen;
        cnt++;
        p = nxt;
    }
    *out = v;
    return cnt;
}

/* ------------------------------------------------------------------ */
/* labels                                                             */
/* ------------------------------------------------------------------ */

typedef struct { const unsigned char *name; long nlen; long no; } Label;

static int is_namech(int c) { return is_alnum_ascii(c); }

static long read_name(const unsigned char *body, long n, long i)
{
    while (i < n && is_namech(body[i])) i++;
    return i;
}

/* Linear search is fine: the shipped programs define a handful. */
static Label *find_label(Label *lab, long nlab, const unsigned char *s,
                         long len)
{
    long k;
    for (k = 0; k < nlab; k++)
        if (lab[k].nlen == len && memcmp(lab[k].name, s, (size_t)len) == 0)
            return &lab[k];
    return 0;
}

/* ------------------------------------------------------------------ */
/* body conversion (convert_body in p88_to_bas.py)                    */
/* ------------------------------------------------------------------ */

static void put_report(Buf *rep, long no, const char *msg)
{
    buf_puts(rep, "line ");
    buf_putl(rep, no);
    buf_puts(rep, ": ");
    buf_puts(rep, msg);
    buf_putc(rep, '\n');
}

/* Python str.strip() whitespace, restricted to latin-1 code points
 * (the str holds chr(byte)): 09-0D, 1C-1F, 20, 85, A0. */
static int py_space(unsigned char c)
{
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) ||
           c == 0x85 || c == 0xA0;
}

/* Skip raw bytes until ':' outside a string, or end of body. */
static long skip_stmt(const unsigned char *body, long n, long i)
{
    int instr = 0;
    for (; i < n; i++) {
        int c = body[i];
        if (instr) { if (c == 0x22) instr = 0; }
        else if (c == 0x22) instr = 1;
        else if (c == 0x3A) break;
    }
    return i;
}

static long next_tok(const unsigned char *body, long n, long i)
{
    while (i < n && body[i] == 0x20) i++;
    return i;
}

/* Convert one line body and append "<text>" to out.  Returns 0 on success,
 * -1 where the Python tool would raise (struct.unpack_from or indexing
 * past the end of the body on a truncated number token). */
static int convert_body(const unsigned char *body, long n, Label *lab,
                        long nlab, long no, long next_no, Buf *out, Buf *rep)
{
    Buf t = { 0, 0, 0, 0 };
    long i = 0, a, z;
    int stmt_start = 1;     /* at start of a statement (STOP-trap drops) */
    int expect_label = 0;   /* 1: after GOTO/GOSUB/RESTORE, 2: THEN/ELSE */
    int in_data = 0, data_quote = 0;
    char msg[128];

    /* label definition at line start: drop "*NAME" and a following ':' */
    if (n > 0 && body[0] == 0xF5) {
        long j = read_name(body, n, 1);
        if (j > 1) {
            i = j;
            if (i < n && body[i] == 0x3A) i++;
            if (i >= n) {           /* label-only line: keep it visible */
                buf_puts(out, "REM *");
                buf_putn(out, (const char *)body + 1, (size_t)(j - 1));
                return 0;
            }
            stmt_start = 1;
        }
    }

    while (i < n) {
        int b = body[i];
        const char *nm;

        /* DATA contents are raw until an unquoted ':' */
        if (in_data && (data_quote || b != 0x3A)) {
            if (b == 0x22) data_quote = !data_quote;
            buf_putc(&t, b); i++; continue;
        }
        if (in_data && b == 0x3A)
            in_data = 0;            /* ':' handled normally below */

        if (b == 0x22) {            /* string literal, raw bytes */
            long j = i + 1;
            buf_putc(&t, '"');
            while (j < n && body[j] != 0x22) buf_putc(&t, body[j++]);
            if (j < n) { buf_putc(&t, '"'); j++; }
            i = j;
            expect_label = 0; stmt_start = 0;
            continue;
        }
        if (b == 0x8F && i + 1 < n && body[i + 1] == 0xE9) {
            /* 8F E9: apostrophe comment, rest of the line is raw text */
            buf_putc(&t, '\'');
            buf_putn(&t, (const char *)body + i + 2, (size_t)(n - i - 2));
            i = n;
            continue;
        }
        if (b == 0xFF) {            /* two-byte FF xx token */
            int tt = (i + 1 < n) ? body[i + 1] : 0;
            if ((nm = ext_name(tt)) != 0) {
                buf_puts(&t, nm); buf_putc(&t, ' ');
            } else if ((nm = func_name(tt)) != 0) {
                buf_puts(&t, nm);
            } else {
                sprintf(msg, "<FF%02X>", tt);
                buf_puts(&t, msg);
                sprintf(msg, "unknown FF token FF%02X", tt);
                put_report(rep, no, msg);
            }
            i += 2;
            expect_label = 0; stmt_start = 0;
            continue;
        }

        /* STOP-trap drops, statement start only; the tokenizer may have
         * kept spaces between the keywords. */
        if (stmt_start && b == 0x90) {             /* STOP ON */
            long j = next_tok(body, n, i + 1);
            if (j < n && body[j] == 0x95) {
                put_report(rep, no, "dropped STOP ON");
                i = j + 1;
                continue;           /* still at statement start */
            }
        }
        if (stmt_start && b == 0x95) {             /* ON STOP GOSUB *x */
            long j = next_tok(body, n, i + 1);
            long k = (j < n) ? next_tok(body, n, j + 1) : n;
            if (j < n && body[j] == 0x90 && k < n && body[k] == 0x8D) {
                put_report(rep, no, "dropped ON STOP GOSUB");
                i = skip_stmt(body, n, k + 1);
                continue;
            }
        }

        if ((nm = tok_name(b)) != 0) {             /* keyword */
            buf_puts(&t, nm);
            if (is_alnum_ascii((unsigned char)nm[strlen(nm) - 1]))
                buf_putc(&t, ' ');
            if (b == 0x84) in_data = 1;
            if (b == 0x89 || b == 0x8D || b == 0x8C) expect_label = 1;
            else if (b == 0xDD || b == 0x9F) expect_label = 2;
            else expect_label = 0;
            stmt_start = (b == 0xDD || b == 0x9F); /* THEN/ELSE start one */
            i++;
            continue;
        }

        /* F5 in GOTO/GOSUB/RESTORE/THEN/ELSE position is a label ref */
        if (b == 0xF5 && expect_label) {
            long j = read_name(body, n, i + 1);
            long nlen = j - (i + 1);
            Label *L = nlen ? find_label(lab, nlab, body + i + 1, nlen) : 0;
            if (L) {
                if (expect_label == 2) buf_puts(&t, "GOTO ");
                buf_putl(&t, L->no);
                i = j;
                expect_label = 0; stmt_start = 0;
                continue;
            }
            /* Unresolved in the shipped program (e.g. MENU.ALP's *TYPE):
             * fall through to the next line and report loudly. */
            buf_puts(rep, "line "); buf_putl(rep, no);
            buf_puts(rep, ": UNRESOLVED label *");
            buf_putn(rep, (const char *)body + i + 1, (size_t)nlen);
            buf_puts(rep, " -> falls through to ");
            buf_putl(rep, next_no); buf_putc(rep, '\n');
            if (expect_label == 2) buf_puts(&t, "GOTO ");
            buf_putl(&t, next_no);
            i = nlen ? j : i + 1;
            expect_label = 0; stmt_start = 0;
            continue;
        }

        if ((nm = op_name(b)) != 0) {              /* operator */
            if (nm[0] >= 'A' && nm[0] <= 'Z') {    /* word ops get spaces */
                buf_putc(&t, ' '); buf_puts(&t, nm); buf_putc(&t, ' ');
            } else {
                buf_puts(&t, nm);
            }
            expect_label = 0; stmt_start = 0;
            i++;
            continue;
        }

        /* Number tokens.  Note they leave expect_label/stmt_start alone. */
        if (b == 0x0B || b == 0x0C || b == 0x0E || b == 0x1C) {
            unsigned v;
            if (i + 3 > n) { free(t.p); return -1; }
            v = rd_u16(body + i + 1);
            if (b == 0x0B) {
                /* Quirk: the Python tool prints &O with the value in
                 * *decimal* (str(), not oct()).  Reproduced verbatim. */
                buf_puts(&t, "&O"); buf_putl(&t, (long)v);
            } else if (b == 0x0C) {
                sprintf(msg, "&H%X", v); buf_puts(&t, msg);
            } else if (b == 0x0E) {     /* line-number reference */
                buf_putl(&t, (long)v);
            } else {                    /* signed int16 */
                buf_putl(&t, v >= 0x8000 ? (long)v - 0x10000L : (long)v);
            }
            i += 3;
        } else if (b == 0x0F) {         /* one-byte unsigned int */
            if (i + 1 >= n) { free(t.p); return -1; }
            buf_putl(&t, (long)body[i + 1]);
            i += 2;
        } else if (b >= 0x11 && b <= 0x1A) {   /* digits 0-9 */
            buf_putc(&t, '0' + (b - 0x11));
            i++;
        } else if (b == 0x1D) {
            put_mbf_single(&t, body + i + 1, n - (i + 1));
            i += 5;
        } else if (b == 0x1F) {
            put_mbf_double(&t, body + i + 1, n - (i + 1));
            i += 9;
        } else if (b == 0x3A) {
            buf_putc(&t, ':');
            i++;
            stmt_start = 1; expect_label = 0;
        } else if (b >= 0x20 && b < 0x7F) {    /* names, punctuation */
            buf_putc(&t, b);
            i++;
            if (b != 0x20) { expect_label = 0; stmt_start = 0; }
        } else {
            sprintf(msg, "<%02X>", b);
            buf_puts(&t, msg);
            sprintf(msg, "unknown byte %02X", b);
            put_report(rep, no, msg);
            i++;
        }
    }

    /* s.strip().strip(':').strip(); empty -> "REM" */
    if (t.oom) { free(t.p); return -1; }
    a = 0; z = (long)t.n;
    while (a < z && py_space((unsigned char)t.p[a])) a++;
    while (z > a && py_space((unsigned char)t.p[z - 1])) z--;
    while (a < z && t.p[a] == ':') a++;
    while (z > a && t.p[z - 1] == ':') z--;
    while (a < z && py_space((unsigned char)t.p[a])) a++;
    while (z > a && py_space((unsigned char)t.p[z - 1])) z--;
    if (a < z) buf_putn(out, t.p + a, (size_t)(z - a));
    else buf_puts(out, "REM");
    free(t.p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* entry point                                                        */
/* ------------------------------------------------------------------ */

char *p88_convert(const unsigned char *data, unsigned long len,
                  unsigned long *out_len, char **report)
{
    Buf out = { 0, 0, 0, 0 }, rep = { 0, 0, 0, 0 };
    Line *lines = 0;
    Label *lab = 0;
    long nlines, nlab = 0, k;
    int fail = 0;

    if (report) *report = 0;
    if (!data && len) return 0;
    if (len > 0x7FFFFFFFUL) return 0;

    nlines = parse_lines(data, (long)len, &lines);
    if (nlines < 0) return 0;

    /* collect_labels: `*NAME` at line start; a later definition of the
     * same name overrides the line number but keeps the first position
     * (Python dict semantics, which the report's dict repr shows). */
    if (nlines > 0) {
        lab = (Label *)malloc((size_t)nlines * sizeof *lab);
        if (!lab) { free(lines); return 0; }
    }
    for (k = 0; k < nlines; k++) {
        const unsigned char *body = data + lines[k].off;
        long n = lines[k].len, j;
        Label *L;
        if (n < 1 || body[0] != 0xF5) continue;
        j = read_name(body, n, 1);
        if (j == 1) continue;
        L = find_label(lab, nlab, body + 1, j - 1);
        if (!L) {
            L = &lab[nlab++];
            L->name = body + 1; L->nlen = j - 1;
        }
        L->no = lines[k].no;
    }

    /* "labels: {'A': 10, 'B': 20}" as Python prints the dict */
    buf_puts(&rep, "labels: {");
    for (k = 0; k < nlab; k++) {
        if (k) buf_puts(&rep, ", ");
        buf_putc(&rep, '\'');
        buf_putn(&rep, (const char *)lab[k].name, (size_t)lab[k].nlen);
        buf_puts(&rep, "': ");
        buf_putl(&rep, lab[k].no);
    }
    buf_puts(&rep, "}\n");

    /* "<no> <text>" lines joined by CRLF, plus a final CRLF (so an empty
     * program yields a lone CRLF, as '\r\n'.join([]) + '\r\n' does). */
    for (k = 0; k < nlines && !fail; k++) {
        long next_no = (k + 1 < nlines) ? lines[k + 1].no : lines[k].no;
        if (k) buf_puts(&out, "\r\n");
        buf_putl(&out, lines[k].no);
        buf_putc(&out, ' ');
        if (convert_body(data + lines[k].off, lines[k].len, lab, nlab,
                         lines[k].no, next_no, &out, &rep) < 0)
            fail = 1;
    }
    buf_puts(&out, "\r\n");

    buf_puts(&rep, "converted: ");
    buf_putl(&rep, nlines);
    buf_puts(&rep, " lines, ");
    buf_putl(&rep, (long)out.n);
    buf_puts(&rep, " bytes\n");

    free(lines);
    free(lab);
    if (fail || out.oom || rep.oom) {
        free(out.p); free(rep.p);
        return 0;
    }
    if (report) *report = rep.p; else free(rep.p);
    if (out_len) *out_len = (unsigned long)out.n;
    return out.p;
}

/* ------------------------------------------------------------------ */
/* standalone test driver                                             */
/* ------------------------------------------------------------------ */

#ifdef P88CONV_TEST
int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *buf = 0;
    size_t n = 0, cap = 0, r;
    char *text, *rep = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <tokenized-file>\n", argv[0]);
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    for (;;) {
        if (n == cap) {
            unsigned char *nb;
            cap = cap ? cap * 2 : 65536;
            nb = (unsigned char *)realloc(buf, cap);
            if (!nb) { fclose(f); free(buf); return 1; }
            buf = nb;
        }
        r = fread(buf + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);

    {
        unsigned long tlen = 0;
        text = p88_convert(buf, (unsigned long)n, &tlen, &rep);
        free(buf);
        if (!text) { fprintf(stderr, "%s: conversion failed\n", argv[1]); return 1; }
        fputs(rep, stderr);
        fwrite(text, 1, tlen, stdout);
    }
    free(text);
    free(rep);
    return 0;
}
#endif
