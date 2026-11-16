/* num.c - number <-> string conversion (N88 style) */
#include "basic.h"
#include <ctype.h>
#include <math.h>

static void fmt_float(double x, int prec, int isdbl, char *out)
{
    char tmp[40], dig[24];
    int n, e, i, neg = 0;
    char *o = out;
    if (x < 0) { neg = 1; x = -x; }
    *o++ = neg ? '-' : ' ';
    if (x == 0) { *o++ = '0'; *o = 0; return; }
    sprintf(tmp, "%.*e", prec - 1, x);
    /* tmp: d.dddde[+-]XX */
    n = 0;
    for (i = 0; tmp[i] && tmp[i] != 'e'; i++) if (isdigit((u8)tmp[i])) dig[n++] = tmp[i];
    dig[n] = 0;
    e = atoi(strchr(tmp, 'e') + 1) + 1;   /* value = 0.dig * 10^e */
    while (n > 1 && dig[n - 1] == '0') dig[--n] = 0;
    if (e > 0 && e <= prec) {
        for (i = 0; i < e; i++) *o++ = (i < n) ? dig[i] : '0';
        if (n > e) { *o++ = '.'; for (i = e; i < n; i++) *o++ = dig[i]; }
        *o = 0;
        return;
    }
    if (e <= 0 && (-e + n) <= prec) {
        *o++ = '.';
        for (i = 0; i < -e; i++) *o++ = '0';
        for (i = 0; i < n; i++) *o++ = dig[i];
        *o = 0;
        return;
    }
    *o++ = dig[0];
    if (n > 1) { *o++ = '.'; for (i = 1; i < n; i++) *o++ = dig[i]; }
    e--;
    *o++ = isdbl ? 'D' : 'E';
    *o++ = e < 0 ? '-' : '+';
    if (e < 0) e = -e;
    sprintf(o, "%02d", e);
}

void num_to_str(VAL *v, char *buf)
{
    switch (v->type) {
    case T_INT: sprintf(buf, "%c%ld", v->u.i < 0 ? '-' : ' ', v->u.i < 0 ? -(long)v->u.i : (long)v->u.i); break;
    case T_SNG: fmt_float((double)v->u.f, 7, 0, buf); break;
    case T_DBL: fmt_float(v->u.d, 16, 1, buf); break;
    default: strcpy(buf, "?"); break;
    }
}

/* parse a numeric constant (no sign). */
int str_to_num(const char *s, VAL *v, int *used)
{
    char tmp[64];
    int t = 0, digits = 0, hasdot = 0, hasexp = 0, expd = 0, lead = 1;
    const char *p = s;
    double d;
    while (*p == ' ') p++;
    while (isdigit((u8)*p) || *p == '.' || *p == ' ') {
        if (*p == ' ') {  /* N88 allows spaces inside numbers when typed; stop if next isn't digit */
            const char *q = p; while (*q == ' ') q++;
            if (!isdigit((u8)*q)) break;
            p = q; continue;
        }
        if (*p == '.') { if (hasdot) break; hasdot = 1; }
        else {
            if (*p != '0' || !lead) { digits++; lead = 0; }
        }
        if (t < 40) tmp[t++] = *p;
        p++;
    }
    if ((*p == 'E' || *p == 'e' || *p == 'D' || *p == 'd')) {
        const char *q = p + 1;
        if (*q == '+' || *q == '-') q++;
        if (isdigit((u8)*q)) {
            hasexp = 1;
            expd = (*p == 'D' || *p == 'd');
            tmp[t++] = 'e';
            p++;
            if (*p == '+' || *p == '-') tmp[t++] = *p++;
            while (isdigit((u8)*p)) { if (t < 60) tmp[t++] = *p; p++; }
        } else if (*q == 0 || !isalnum((u8)*q)) {
            /* "1E" alone -> treat as 1E0 */
            hasexp = 1; expd = (*p == 'D' || *p == 'd'); p++;
            if (*p == '+' || *p == '-') p++;
        }
    }
    tmp[t] = 0;
    d = strtod(tmp, 0);
    if (*p == '%') {
        p++;
        if (d > 32767.0 || d < -32768.0) berror(E_OVERFLOW);
        v->type = T_INT; v->u.i = (i16)round_away(d);
    } else if (*p == '!') {
        p++; v->type = T_SNG; v->u.f = (float)d;
    } else if (*p == '#') {
        p++; v->type = T_DBL; v->u.d = d;
    } else if (expd) {
        v->type = T_DBL; v->u.d = d;
    } else if (hasexp) {
        v->type = T_SNG; v->u.f = (float)d;
    } else if (!hasdot && d <= 32767.0) {
        v->type = T_INT; v->u.i = (i16)d;
    } else if (digits > 7) {
        v->type = T_DBL; v->u.d = d;
    } else {
        v->type = T_SNG; v->u.f = (float)d;
    }
    if (v->type == T_SNG && (d > 1.70141e38 || d < -1.70141e38)) berror(E_OVERFLOW);
    if (used) *used = (int)(p - s);
    return 1;
}
