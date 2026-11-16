/* var.c - variables and arrays */
#include "basic.h"
#include <ctype.h>

typedef struct { u16 len; char *p; } SD;

struct VAR {
    VAR *next;
    u8 type;
    u8 isarray;
    u8 ndim;
    char *name;
    union { i16 i; float f; double d; SD s; } v;
    int *dims;        /* upper bounds */
    void *data;
    long count;
};

#define NHASH 64
#define VC 128
static struct { u8 *pos; u8 *end; VAR *v; } vcache[VC];
extern u8 *prog_mem;

void var_cache_clear(void) { memset(vcache, 0, sizeof(vcache)); }
static VAR *htab[NHASH];
static u8 deftype[26];
int option_base = 0;
static int base_fixed = 0;

static unsigned hashname(const char *n, int type, int arr)
{
    unsigned h = (unsigned)type * 7 + arr * 13;
    while (*n) h = h * 31 + (u8)*n++;
    return h % NHASH;
}

void var_cache_clear(void);
int default_type(char c)
{
    c = (char)toupper((u8)c);
    if (c < 'A' || c > 'Z') return T_SNG;
    return deftype[c - 'A'] ? deftype[c - 'A'] : T_SNG;
}

void set_deftype(char from, char to, int type)
{
    int c;
    from = (char)toupper((u8)from); to = (char)toupper((u8)to);
    if (from < 'A' || from > 'Z' || to < from || to > 'Z') syntax();
    for (c = from; c <= to; c++) deftype[c - 'A'] = (u8)type;
    var_cache_clear();
}

static int tsize(int type)
{
    switch (type) { case T_INT: return 2; case T_SNG: return 4; case T_DBL: return 8; default: return sizeof(SD); }
}

static VAR *lookup(const char *name, int type, int arr)
{
    VAR *v;
    for (v = htab[hashname(name, type, arr)]; v; v = v->next)
        if (v->type == type && v->isarray == arr && strcmp(v->name, name) == 0) return v;
    return 0;
}

static VAR *create(const char *name, int type, int arr)
{
    unsigned h = hashname(name, type, arr);
    VAR *v = (VAR *)calloc(1, sizeof(VAR));
    if (!v) berror(E_OUT_OF_MEM);
    v->name = (char *)malloc(strlen(name) + 1);
    if (!v->name) { free(v); berror(E_OUT_OF_MEM); }
    strcpy(v->name, name);
    v->type = (u8)type; v->isarray = (u8)arr;
    v->next = htab[h]; htab[h] = v;
    return v;
}

VAR *var_find(const char *name, int type, int create_it)
{
    VAR *v = lookup(name, type, 0);
    if (!v && create_it) v = create(name, type, 0);
    return v;
}

/* parse a variable name at txtptr. name buffer >= 41 bytes. */
int parse_varname(char *name, int *type)
{
    int n = 0;
    u8 c = peekc();
    if (!isalpha(c)) syntax();
    while (isalnum(*txtptr) || *txtptr == '.') {
        if (n < 40) name[n++] = (char)toupper(*txtptr);
        txtptr++;
    }
    name[n] = 0;
    switch (*txtptr) {
    case '%': *type = T_INT; txtptr++; break;
    case '!': *type = T_SNG; txtptr++; break;
    case '#': *type = T_DBL; txtptr++; break;
    case '$': *type = T_STR; txtptr++; break;
    default: *type = default_type(name[0]); break;
    }
    return n;
}

static void free_strings(void *data, long count)
{
    SD *s = (SD *)data; long i;
    for (i = 0; i < count; i++) if (s[i].p) free(s[i].p);
}

void dim_array(const char *name, int type, int ndim, int *dims)
{
    VAR *v;
    long count = 1, bytes;
    int i;
    if (lookup(name, type, 1)) berror(E_DUP_DEF);
    for (i = 0; i < ndim; i++) {
        if (dims[i] < option_base) berror(E_SUBSCRIPT);
        count *= (long)(dims[i] - option_base + 1);
        if (count > 65535L) berror(E_OUT_OF_MEM);
    }
    bytes = count * tsize(type);
    if (bytes > 65000L) berror(E_OUT_OF_MEM);
    v = create(name, type, 1);
    v->ndim = (u8)ndim;
    v->dims = (int *)malloc(sizeof(int) * ndim);
    v->data = calloc((size_t)count, tsize(type));
    if (!v->dims || !v->data) {
        /* unlink */
        unsigned h = hashname(name, type, 1);
        htab[h] = v->next;
        if (v->dims) free(v->dims);
        if (v->data) free(v->data);
        free(v->name); free(v);
        berror(E_OUT_OF_MEM);
    }
    memcpy(v->dims, dims, sizeof(int) * ndim);
    v->count = count;
    base_fixed = 1;
}

void var_erase(const char *name, int type)
{
    unsigned h = hashname(name, type, 1);
    VAR **pp = &htab[h], *v;
    while ((v = *pp) != 0) {
        if (v->type == type && v->isarray && strcmp(v->name, name) == 0) {
            var_cache_clear();
            *pp = v->next;
            if (type == T_STR) free_strings(v->data, v->count);
            free(v->data); free(v->dims); free(v->name); free(v);
            return;
        }
        pp = &v->next;
    }
    berror(E_ILLEGAL_FN);
}

void var_clear_all(void)
{
    int i;
    for (i = 0; i < NHASH; i++) {
        VAR *v = htab[i], *n;
        while (v) {
            n = v->next;
            if (v->isarray) {
                if (v->type == T_STR) free_strings(v->data, v->count);
                free(v->data); free(v->dims);
            } else if (v->type == T_STR && v->v.s.p) free(v->v.s.p);
            free(v->name); free(v);
            v = n;
        }
        htab[i] = 0;
    }
    memset(deftype, 0, sizeof(deftype));
    option_base = 0;
    base_fixed = 0;
    var_cache_clear();
}

void option_base_set(int b)
{
    if (base_fixed && b != option_base) berror(E_DUP_DEF);
    option_base = b;
    base_fixed = 1;
}

/* parse variable reference at txtptr; returns pointer to storage */
void *var_ptr_for(u8 **pp, int *ptype, int create_array_auto)
{
    char name[41];
    int type, subs[8], n = 0, i;
    VAR *v = 0;
    long idx;
    u8 *start;
    unsigned ci = 0;
    int cacheable;
    (void)pp; (void)create_array_auto;
    start = txtptr;
    while (*start == ' ') start++;
    txtptr = start;
    cacheable = (start >= prog_mem && start < prog_mem + 0xFF00u);
    if (cacheable) {
        ci = (unsigned)(start - prog_mem) % VC;
        if (vcache[ci].pos == start) {
            v = vcache[ci].v;
            txtptr = vcache[ci].end;
            type = v->type;
            *ptype = type;
            if (!v->isarray) return &v->v;
            PEEKC();
            txtptr++;  /* '(' */
            goto subscripts;
        }
    }
    parse_varname(name, &type);
    *ptype = type;
    if (PEEKC() == '(' ) {
        u8 *after_name = txtptr;
        v = lookup(name, type, 1);
        if (!v) {
            /* need the subscript count to auto-dim: parse first */
            u8 *save = txtptr;
            int cnt = 1, depth = 0;
            u8 *p = txtptr + 1;
            /* count top-level commas */
            while (*p) {
                if (*p == '(') depth++;
                else if (*p == ')') { if (depth == 0) break; depth--; }
                else if (*p == ',' && depth == 0) cnt++;
                else if (*p == '"') { p++; while (*p && *p != '"') p++; if (!*p) break; }
                else if (*p == TK_FUNC) p++;
                else if (*p == TK_INT || *p == TK_LINENUM || *p == TK_HEX || *p == TK_OCT) p += 2;
                else if (*p == TK_SNG) p += 4;
                else if (*p == TK_DBL) p += 8;
                p++;
            }
            {
                int d[8];
                if (cnt > 8) syntax();
                for (i = 0; i < cnt; i++) d[i] = 10;
                dim_array(name, type, cnt, d);
                v = lookup(name, type, 1);
            }
            txtptr = save;
        }
        if (cacheable) { vcache[ci].pos = start; vcache[ci].end = after_name; vcache[ci].v = v; }
        txtptr++;
subscripts:
        for (;;) {
            if (n >= 8) syntax();
            subs[n++] = eval_int();
            if (accept(',')) continue;
            expect(')');
            break;
        }
        if (v->ndim != n) berror(E_SUBSCRIPT);
        idx = 0;
        for (i = 0; i < n; i++) {
            if (subs[i] < option_base || subs[i] > v->dims[i]) berror(E_SUBSCRIPT);
            idx = idx * (v->dims[i] - option_base + 1) + (subs[i] - option_base);
        }
        return (u8 *)v->data + idx * tsize(type);
    }
    v = lookup(name, type, 0);
    if (!v) v = create(name, type, 0);
    if (cacheable) { vcache[ci].pos = start; vcache[ci].end = txtptr; vcache[ci].v = v; }
    return &v->v;
}

/* returns array VAR for name (for ERASE / GET@ etc). */
void *array_base(const char *name, int type, long *bytes)
{
    VAR *v = lookup(name, type, 1);
    if (!v) return 0;
    if (bytes) *bytes = v->count * tsize(type);
    return v->data;
}

void var_get(void *p, int type, VAL *v)
{
    v->type = (u8)type;
    switch (type) {
    case T_INT: v->u.i = *(i16 *)p; break;
    case T_SNG: v->u.f = *(float *)p; break;
    case T_DBL: v->u.d = *(double *)p; break;
    case T_STR: v->u.s.len = ((SD *)p)->len; v->u.s.p = ((SD *)p)->p; break;
    }
}

void var_set(void *p, int type, VAL *v)
{
    if (type == T_STR) {
        SD *s = (SD *)p;
        char *np = 0;
        if (v->type != T_STR) berror(E_TYPE);
        if (v->u.s.len) {
            np = (char *)malloc(v->u.s.len);
            if (!np) berror(E_OUT_STR);
            memcpy(np, v->u.s.p, v->u.s.len);
        }
        if (s->p) free(s->p);
        s->p = np; s->len = v->u.s.len;
        return;
    }
    if (v->type == T_STR) berror(E_TYPE);
    val_conv(v, type);
    switch (type) {
    case T_INT: *(i16 *)p = v->u.i; break;
    case T_SNG: *(float *)p = v->u.f; break;
    case T_DBL: *(double *)p = v->u.d; break;
    }
}
