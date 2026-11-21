/* p88conv.c - PC-88 tokenized Disk BASIC -> BASIC source text converter
 *
 * C port of tools/p88_to_bas.py (token tables shared with
 * tools/p88_detok.py; format notes in docs/TOKEN-FORMAT.md).  The Python
 * tool is the mirror: the text produced here must match the file it writes
 * byte for byte (tools/p88conv_test.py compares the two).  Change one side
 * only together with the other, or the two drift apart.
 *
 * Labels pass through unchanged: `*NAME` definitions and references are
 * emitted as written and resolved by the interpreter's own label support
 * (token.c / exec.c).  A reference to a label this program does not define
 * is reported and still passed through; the interpreter raises "undefined
 * line" if it is ever executed, as the PC-88 does.  Names are read and
 * matched by the interpreter's rules (see "labels" below), and a keyword
 * token that directly follows a label gets a separator (':' after a
 * definition, a space after a reference) so the interpreter does not read
 * it into the name.
 *
 * Numeric constants are spelled so the interpreter reads back the token's
 * type and exact value, negative ones in parentheses (see "numbers").
 *
 * Adaptations made while converting (P8PBASIC lacks these PC-88 features):
 *   - STOP traps: `STOP ON` and `ON STOP GOSUB *x` are dropped and
 *     reported (documented approximation: docs/APPROXIMATIONS.md item 26).
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

/* FF xx function tokens.  The converter commits to INT for FF85 (the
 * detokenizer still shows it with a '?').  FF83 = MID$ and FFDC = TIME$
 * are confirmed by FC88V42 MENU.ALP (line 286 against the plain-text twin
 * in line 460); the interpreter tokenizes both names as written. */
static const char *func_name(int t)
{
    switch (t) {
    case 0x82: return "RIGHT$";
    case 0x83: return "MID$";
    case 0x85: return "INT";
    case 0x90: return "INP";
    case 0x92: return "LEN";
    case 0x93: return "STR$";
    case 0x94: return "VAL";
    case 0x97: return "PEEK";
    case 0x9A: return "HEX$";
    case 0xDC: return "TIME$";
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

/* Operators F0-FE.  F5 doubles as the label marker (line start and jump
 * target position); both uses come out as '*'. */
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

/* Does the nd-digit decimal d.ddd * 10^exp10 read back as exactly x? */
static int reads_back(const char *dig, int nd, int exp10, double x)
{
    char t[48];
    int k, o = 0;
    t[o++] = dig[0];
    t[o++] = '.';
    for (k = 1; k < nd; k++) t[o++] = dig[k];
    t[o++] = '0';
    sprintf(t + o, "e%d", exp10);
    return strtod(t, 0) == x;
}

/* Format a positive finite double exactly as Python's repr() does: the
 * shortest digit string that reads back to the same double (and of those
 * the one nearest the value), fixed notation for 1e-4 <= x < 1e16 (with a
 * trailing ".0" when integral), otherwise d.ddde[+-]XX.  out needs 32
 * bytes.
 *
 * For each length nd = 1..17 the candidates are the correctly rounded
 * nd-digit decimal ("%.*e") and its two neighbours one unit in the last
 * place away.  The rounded one alone is not enough: the set of decimals
 * that read back to x is an interval around x, lopsided when x is a power
 * of two, so the nearest nd-digit decimal can fall outside it while the
 * one on the other side of x is inside (2^-24 = 5.9604644775390625e-08:
 * 16 digits round to ...062, which misses; ...063 reads back).  At most
 * one candidate can succeed when the rounded one fails, and when the
 * rounded one succeeds it is the nearest, so the first hit is repr()'s
 * answer.  17 digits always read back. */
static void fmt_pyrepr(double x, char *out)
{
    char tmp[64], dig[24], alt[24];
    int nd, exp10 = 0, decpt, k, o = 0;
    const char *s;

    for (nd = 1; nd <= 17; nd++) {
        int n = 0, e2;
        snprintf(tmp, sizeof tmp, "%.*e", nd - 1, x);
        for (s = tmp; *s && *s != 'e'; s++)
            if (*s >= '0' && *s <= '9' && n < 17) dig[n++] = *s;
        exp10 = atoi(s + 1);
        if (reads_back(dig, nd, exp10, x)) break;

        memcpy(alt, dig, (size_t)nd);           /* one unit up */
        e2 = exp10;
        for (k = nd - 1; k >= 0 && alt[k] == '9'; k--) alt[k] = '0';
        if (k >= 0) alt[k]++;
        else { alt[0] = '1'; e2++; }            /* 99..9 -> 10..0 */
        if (reads_back(alt, nd, e2, x)) {
            memcpy(dig, alt, (size_t)nd); exp10 = e2; break;
        }

        memcpy(alt, dig, (size_t)nd);           /* one unit down */
        e2 = exp10;
        for (k = nd - 1; k > 0 && alt[k] == '0'; k--) alt[k] = '9';
        alt[k]--;
        if (alt[0] == '0') {                    /* 10..0 -> 99..9 */
            for (k = 0; k < nd; k++) alt[k] = '9';
            e2--;
        }
        if (reads_back(alt, nd, e2, x)) {
            memcpy(dig, alt, (size_t)nd); exp10 = e2; break;
        }
    }
    if (nd > 17) nd = 17;       /* not reached: 17 digits read back */
    while (nd > 1 && dig[nd - 1] == '0') nd--;
    decpt = exp10 + 1;          /* digits are 0.DDDD * 10^decpt */

    if (decpt > -4 && decpt <= 16) {
        if (decpt <= 0) {
            out[o++] = '0'; out[o++] = '.';
            for (k = 0; k < -decpt; k++) out[o++] = '0';
            for (k = 0; k < nd; k++) out[o++] = dig[k];
        } else if (decpt >= nd) {
            for (k = 0; k < nd; k++) out[o++] = dig[k];
            for (k = nd; k < decpt; k++) out[o++] = '0';
            out[o++] = '.'; out[o++] = '0';
        } else {
            for (k = 0; k < decpt; k++) out[o++] = dig[k];
            out[o++] = '.';
            for (k = decpt; k < nd; k++) out[o++] = dig[k];
        }
        out[o] = 0;
    } else {
        out[o++] = dig[0];
        if (nd > 1) {
            out[o++] = '.';
            for (k = 1; k < nd; k++) out[o++] = dig[k];
        }
        sprintf(out + o, "e%c%02d", exp10 < 0 ? '-' : '+',
                exp10 < 0 ? -exp10 : exp10);
    }
}

/* Spelling of numeric constants (p88_detok.py: _typed / num_literal /
 * int_literal; the two must stay byte-identical).  The text is read back
 * by the interpreter, whose parser (num.c str_to_num) takes the type from
 * the spelling:
 *   suffix % / ! / #        -> integer / single / double
 *   exponent letter D       -> double
 *   exponent letter E       -> single
 *   no '.', value <= 32767  -> integer
 *   more than 7 digits      -> double  (digits counted without leading
 *                                       zeros)
 *   otherwise               -> single
 * and a single literal above 1.70141E+38 is an Overflow error.  The
 * digits are the repr() digits, so the value read back is exact; the
 * letter / suffix is chosen so the type read back is the token's type:
 *   single: plain form as is when it has <= 7 digits, else with '!';
 *           exponent form with 'E'; above the literal limit the exact
 *           single product (half*2), which the interpreter's single
 *           multiply does not range-check.
 *   double: plain form as is when it has > 7 digits, else with '#';
 *           exponent form with 'D'.
 * Negative constants are parenthesized: the interpreter's unary minus
 * binds looser than '^', so a bare "-2.5^2" would read as -(2.5^2). */
#define SNG_LITERAL_MAX 1.70141e38

static void put_typed(Buf *b, double a, int dbl)    /* a > 0 */
{
    char r[32], *e;
    const char *s;
    int nd = 0, lead = 1;

    fmt_pyrepr(a, r);
    e = strchr(r, 'e');
    if (e) {
        *e = dbl ? 'D' : 'E';
        buf_puts(b, r);
        return;
    }
    for (s = r; *s; s++) {                  /* the parser's digit count */
        if (*s == '.') continue;
        if (*s != '0' || !lead) { nd++; lead = 0; }
    }
    buf_puts(b, r);
    if (dbl) { if (nd <= 7) buf_putc(b, '#'); }
    else if (nd > 7) buf_putc(b, '!');
}

static void put_num_literal(Buf *b, double v, int dbl)  /* v != 0 */
{
    double a = v < 0 ? -v : v;
    if (v < 0) buf_puts(b, "(-");
    if (!dbl && a > SNG_LITERAL_MAX) {
        buf_putc(b, '(');
        put_typed(b, a * 0.5, 0);
        buf_puts(b, "*2)");
    } else {
        put_typed(b, a, dbl);
    }
    if (v < 0) buf_putc(b, ')');
}

/* Signed 16-bit integer constant.  -32768 has no integer literal (32768
 * reads as a single), so it is spelled as integer arithmetic that stays
 * integer. */
static void put_int16(Buf *b, long v)
{
    if (v >= 0) { buf_putl(b, v); return; }
    if (v == -32768L) { buf_puts(b, "(-32767-1)"); return; }
    buf_putc(b, '(');
    buf_putl(b, v);
    buf_putc(b, ')');
}

static double scale2(double v, int e);

/* MBF numbers are sign * 0.1fff...(binary) * 2^(exp-128): the mantissa is
 * a fraction in [0.5, 1) whose leading 1 is implied (its bit position
 * holds the sign instead), and exp == 0 means zero.  1.0 is therefore
 * exponent 0x81 with an all-zero stored mantissa.
 *
 * MBF single: [m0][m1][m2: sign|m2][exp].  24-bit mantissa with the hidden
 * bit at bit 23:
 *   bits  = m0 | m1<<8 | (m2 & 0x7F)<<16 | 0x800000
 *   value = sign * bits * 2^(exp-128-24)
 *         = sign * (1 + frac/2^23) * 2^(exp-129)
 * which is exact in a double.  Zero keeps its type ("0!").  Short slice
 * -> "?". */
static void put_mbf_single(Buf *b, const unsigned char *p, long avail)
{
    double bits;
    if (avail < 4) { buf_putc(b, '?'); return; }
    if (p[3] == 0) { buf_puts(b, "0!"); return; }
    bits = (double)((unsigned long)p[0] | ((unsigned long)p[1] << 8) |
                    ((unsigned long)(p[2] & 0x7F) << 16) | 0x800000UL);
    if (p[2] & 0x80) bits = -bits;
    put_num_literal(b, scale2(bits, (int)p[3] - 128 - 24), 0);
}

/* MBF double: [m0..m5][m6: sign|m6][exp].  56-bit mantissa with the hidden
 * bit at bit 55:
 *   value = sign * bits * 2^(exp-128-56)
 * Python converts the int to float first (round-half-even to 53 bits),
 * which the uint64 -> double conversion here also does.  That rounding is
 * a representation limit shared with the interpreter, whose double is a
 * binary64 (docs/TOKEN-FORMAT.md).  Zero keeps its type ("0#"). */
static void put_mbf_double(Buf *b, const unsigned char *p, long avail)
{
    unsigned long long frac;
    double v;
    int k;
    if (avail < 8) { buf_putc(b, '?'); return; }
    if (p[7] == 0) { buf_puts(b, "0#"); return; }
    frac = (unsigned long long)(p[6] & 0x7F) << 48;
    for (k = 5; k >= 0; k--) frac |= (unsigned long long)p[k] << (8 * k);
    frac |= 1ULL << 55;
    v = (double)frac;
    if (p[6] & 0x80) v = -v;
    put_num_literal(b, scale2(v, (int)p[7] - 128 - 56), 1);
}

/* v * 2^e without libm (the acceptance build links no -lm).  Each step is
 * an exact power-of-two scaling; for MBF ranges (|e| <= 183, mantissa
 * >= 2^23) the value stays well inside the normal double range. */
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
 * last line, framed by last_line_end().  Returns the line count, or -1 on
 * allocation failure. */
static const int link_probe[5] = { 0, -1, 1, -2, 2 };

/* End (exclusive) of the body of the last line, whose body starts at off.
 * The line's own link field points just past its 00 terminator, at the
 * 00 00 end-of-program marker, so the body ends at link-1; number
 * payloads may contain 00 bytes, so the body is never scanned for one.
 * The link drifts like any other, hence the probes: first a terminator
 * followed by the end marker, then (file cut short after the terminator)
 * a terminator alone.  Only when the link leads nowhere does the first 00
 * decide. */
static long last_line_end(const unsigned char *d, long n, long off, long link)
{
    const unsigned char *z;
    int k;
    for (k = 0; k < 5; k++) {
        long cand = link + link_probe[k];
        if (cand < off + 1 || cand + 2 > n) continue;
        if (d[cand - 1] == 0 && d[cand] == 0 && d[cand + 1] == 0)
            return cand - 1;
    }
    for (k = 0; k < 5; k++) {
        long cand = link + link_probe[k];
        if (cand < off + 1 || cand > n) continue;
        if (d[cand - 1] == 0) return cand - 1;
    }
    z = (const unsigned char *)memchr(d + off, 0, (size_t)(n - off));
    return z ? (long)(z - d) : n;
}

static long parse_lines(const unsigned char *d, long n, Line **out)
{
    static const int *const delta = link_probe;
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
            long end = last_line_end(d, n, off, link);
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

/* Label names follow the interpreter's grammar, because the interpreter
 * is what resolves them in the converted text:
 *   - a name is a letter followed by letters, digits and '.' (token.c
 *     label scan, exec.c label_char); `*100` is not a label there
 *     (GOTO *100 is a syntax error);
 *   - the tokenizer upper-cases definitions and references alike, so
 *     lookup ignores case;
 *   - a definition is `*NAME` as the first thing on a line, leading
 *     spaces allowed (find_label); the whole name must match (*SUB does
 *     not find *SUB.1), and the first definition of a name wins. */
typedef struct { const unsigned char *name; long nlen; long no; } Label;

static int is_alpha_ascii(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int is_labch(int c) { return is_alnum_ascii(c) || c == '.'; }

static int fold(int c) { return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c; }

/* End of the run of label characters at body[i..]. */
static long read_run(const unsigned char *body, long n, long i)
{
    while (i < n && is_labch(body[i])) i++;
    return i;
}

static long next_tok(const unsigned char *body, long n, long i);

/* The label a line body defines: its name is body[*start .. return);
 * returns 0 when the line defines none. */
static long label_def(const unsigned char *body, long n, long *start)
{
    long i = next_tok(body, n, 0);
    if (i + 1 < n && body[i] == 0xF5 && is_alpha_ascii(body[i + 1])) {
        *start = i + 1;
        return read_run(body, n, i + 1);
    }
    return 0;
}

/* True when body[i] is a keyword token: its text starts with a letter and
 * would be read by the interpreter's tokenizer as more of a label name
 * written directly before it (`*ABC` + PRINT -> label ABCPRINT). */
static int glues(const unsigned char *body, long n, long i)
{
    const char *nm;
    if (i >= n) return 0;
    if (body[i] == 0xFF) {
        int t = (i + 1 < n) ? body[i + 1] : 0;
        return ext_name(t) != 0 || func_name(t) != 0;
    }
    nm = tok_name(body[i]);
    return nm != 0 && is_alnum_ascii((unsigned char)nm[0]);
}

/* Case-insensitive.  Linear search is fine: the shipped programs define a
 * handful. */
static Label *find_label(Label *lab, long nlab, const unsigned char *s,
                         long len)
{
    long k, j;
    for (k = 0; k < nlab; k++) {
        if (lab[k].nlen != len) continue;
        for (j = 0; j < len; j++)
            if (fold(lab[k].name[j]) != fold(s[j])) break;
        if (j == len) return &lab[k];
    }
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
                        long nlab, long no, Buf *out, Buf *rep)
{
    Buf t = { 0, 0, 0, 0 };
    long i = 0, a, z;
    int stmt_start = 1;     /* at start of a statement (STOP-trap drops) */
    int expect_label = 0;   /* after GOTO/GOSUB/RESTORE/THEN/ELSE */
    int in_data = 0, data_quote = 0;
    char msg[128];

    /* label definition at line start: pass "*NAME" through.  What follows
     * is still the start of a statement, with or without a ':'.  A
     * keyword token directly after the name gets the ':' the
     * interpreter's tokenizer needs to end the name there
     * (`*NAME:STATEMENT`, the form MA2026 itself uses); a definition
     * already followed by ':' or a space is left as written. */
    {
        long s0 = 0, j = label_def(body, n, &s0);
        if (j) {
            buf_putc(&t, '*');
            buf_putn(&t, (const char *)body + s0, (size_t)(j - s0));
            if (glues(body, n, j)) buf_putc(&t, ':');
            i = j;
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
            expect_label = (b == 0x89 || b == 0x8D || b == 0x8C ||
                            b == 0xDD || b == 0x9F);
            stmt_start = (b == 0xDD || b == 0x9F); /* THEN/ELSE start one */
            i++;
            continue;
        }

        /* F5 in GOTO/GOSUB/RESTORE/THEN/ELSE position is a label ref:
         * pass "*NAME" through.  One the program does not define (e.g.
         * MENU.ALP's *TYPE) is reported; the interpreter raises
         * "undefined line" if that statement is ever executed.  A name
         * that does not start with a letter (`*100`) is not a label to
         * the interpreter at all (syntax error) and is reported the
         * same way.  A keyword token directly after the name is set off
         * with a space so the tokenizer does not read it into the name.
         * (F5 with no name after it is left to the operator case
         * below.) */
        if (b == 0xF5 && expect_label) {
            long j = read_run(body, n, i + 1);
            long nlen = j - (i + 1);
            if (nlen) {
                buf_putc(&t, '*');
                buf_putn(&t, (const char *)body + i + 1, (size_t)nlen);
                if (!is_alpha_ascii(body[i + 1]) ||
                    !find_label(lab, nlab, body + i + 1, nlen)) {
                    buf_puts(rep, "line "); buf_putl(rep, no);
                    buf_puts(rep, ": UNDEFINED label *");
                    buf_putn(rep, (const char *)body + i + 1, (size_t)nlen);
                    buf_puts(rep, " (passed through)\n");
                }
                if (glues(body, n, j)) buf_putc(&t, ' ');
                i = j;
                stmt_start = 0;     /* expect_label stays for "*A,*B" */
                continue;
            }
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
                sprintf(msg, "&O%o", v); buf_puts(&t, msg);
            } else if (b == 0x0C) {
                sprintf(msg, "&H%X", v); buf_puts(&t, msg);
            } else if (b == 0x0E) {     /* line-number reference */
                buf_putl(&t, (long)v);
            } else {                    /* signed int16 */
                put_int16(&t, v >= 0x8000 ? (long)v - 0x10000L : (long)v);
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
            if (b != 0x20) {
                /* ',' keeps the label position: ON x GOSUB *A,*B */
                if (b != 0x2C) expect_label = 0;
                stmt_start = 0;
            }
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

    /* collect_labels: the definitions the interpreter can find, for the
     * report and for the undefined-reference check only (the text keeps
     * the labels).  The first definition of a name wins, as in the
     * interpreter's lookup. */
    if (nlines > 0) {
        lab = (Label *)malloc((size_t)nlines * sizeof *lab);
        if (!lab) { free(lines); return 0; }
    }
    for (k = 0; k < nlines; k++) {
        const unsigned char *body = data + lines[k].off;
        long s0 = 0, j = label_def(body, lines[k].len, &s0);
        if (!j || find_label(lab, nlab, body + s0, j - s0)) continue;
        lab[nlab].name = body + s0;
        lab[nlab].nlen = j - s0;
        lab[nlab].no = lines[k].no;
        nlab++;
    }

    /* "labels: {'A': 10, 'B': 20}" as Python prints the dict (names
     * folded to upper case, as the interpreter stores them) */
    buf_puts(&rep, "labels: {");
    for (k = 0; k < nlab; k++) {
        long j;
        if (k) buf_puts(&rep, ", ");
        buf_putc(&rep, '\'');
        for (j = 0; j < lab[k].nlen; j++) buf_putc(&rep, fold(lab[k].name[j]));
        buf_puts(&rep, "': ");
        buf_putl(&rep, lab[k].no);
    }
    buf_puts(&rep, "}\n");

    /* "<no> <text>" lines joined by CRLF, plus a final CRLF (so an empty
     * program yields a lone CRLF, as '\r\n'.join([]) + '\r\n' does). */
    for (k = 0; k < nlines && !fail; k++) {
        if (k) buf_puts(&out, "\r\n");
        buf_putl(&out, lines[k].no);
        buf_putc(&out, ' ');
        if (convert_body(data + lines[k].off, lines[k].len, lab, nlab,
                         lines[k].no, &out, &rep) < 0)
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
