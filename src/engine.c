/* engine.c - see engine.h. The interpreter's exit path is the
 * engine's stop path: when a program run finishes, exec.c calls
 * hal_term() and then exit(0); halp8's hal_term longjmps back here
 * instead (p8internal.h), so the thread ends cleanly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "basic.h"
#include "engine.h"
#include "halp8.h"
#include "p8internal.h"
#include "p88disk.h"
#include "p88conv.h"
#include "opna.h"

void interp_init(void);
void exec_direct_pending_set(const char *s);

jmp_buf p8_bail_jmp;
volatile int p8_bail_armed = 0;

void p8_bail(void)
{
    if (p8_bail_armed) longjmp(p8_bail_jmp, 1);
}

#define RING_FRAMES (44100 * 2)     /* 2 seconds of stereo audio */
struct P8Engine {
    char *d88_path;
    char *workdir;
    char bootfile[32];
    pthread_t th;
    volatile int running;
    int started;
    short ring[RING_FRAMES * 2];
    int ring_head, ring_tail;
    unsigned long dropped;
    unsigned long rendered;         /* all frames the sink ever saw */
    struct timespec t0;             /* p8_start wall clock */
    pthread_mutex_t ring_mu;
};

static P8Engine *g_engine = 0;      /* single instance, see engine.h */

double p8_audio_seconds(void)
{
    if (!g_engine) return 0;
    return (double)g_engine->rendered / 44100.0;
}

double p8_wall_seconds(void)
{
    struct timespec ts;
    if (!g_engine) return 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)(ts.tv_sec - g_engine->t0.tv_sec) +
           (double)(ts.tv_nsec - g_engine->t0.tv_nsec) / 1e9;
}

static void ring_sink(const short *frames, int count, void *ctx)
{
    P8Engine *e = (P8Engine *)ctx;
    int i;
    pthread_mutex_lock(&e->ring_mu);
    e->rendered += (unsigned long)count;
    for (i = 0; i < count; i++) {
        int next = (e->ring_tail + 1) % RING_FRAMES;
        if (next == e->ring_head) { e->dropped++; continue; }
        e->ring[e->ring_tail * 2] = frames[i * 2];
        e->ring[e->ring_tail * 2 + 1] = frames[i * 2 + 1];
        e->ring_tail = next;
    }
    pthread_mutex_unlock(&e->ring_mu);
}

int p8_audio_read(P8Engine *e, short *out, int frames)
{
    int n = 0;
    pthread_mutex_lock(&e->ring_mu);
    while (n < frames && e->ring_head != e->ring_tail) {
        out[n * 2] = e->ring[e->ring_head * 2];
        out[n * 2 + 1] = e->ring[e->ring_head * 2 + 1];
        e->ring_head = (e->ring_head + 1) % RING_FRAMES;
        n++;
    }
    pthread_mutex_unlock(&e->ring_mu);
    return n;
}

unsigned long p8_audio_dropped(P8Engine *e) { return e->dropped; }

P8Engine *p8_create(const char *d88_path)
{
    P8Engine *e = (P8Engine *)calloc(1, sizeof(P8Engine));
    if (!e) return 0;
    e->d88_path = strdup(d88_path);
    g_engine = e;
    pthread_mutex_init(&e->ring_mu, 0);
    /* the vessel opens the disk lazily from the environment */
    setenv("P8P_DISK", d88_path, 1);
    opna_reset();
    opna_set_sink(ring_sink, e);
    return e;
}

void p8_destroy(P8Engine *e)
{
    if (!e) return;
    if (e->started) p8_stop(e);
    opna_set_sink(0, 0);
    free(e->d88_path);
    free(e->workdir);
    free(e);
}

/* Write the disk's files into a fresh scratch directory; BASIC
 * programs (attr 0x80) become <base>.BAS text. Returns 0 and sets
 * e->workdir / e->bootfile on success. */
static int materialize(P8Engine *e)
{
    P88FileInfo files[64];
    static unsigned char buf[1 << 20];
    char tmpl[256];
    const char *tmp = getenv("TMPDIR");
    int n, i;
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/p8pXXXXXX", tmp);
    if (!mkdtemp(tmpl)) return -1;
    e->workdir = strdup(tmpl);
    n = p88disk_list(files, 64);
    if (n < 0) return -1;
    e->bootfile[0] = 0;
    for (i = 0; i < n; i++) {
        long sz = p88disk_read(files[i].name, buf, sizeof(buf));
        char path[320], base[16];
        const char *dot;
        FILE *fp;
        if (sz < 0) continue;
        /* base name without extension, for the .BAS materialization */
        memset(base, 0, sizeof(base));
        strncpy(base, files[i].name, sizeof(base) - 1);
        dot = strrchr(base, '.');
        if (files[i].attr & 0x80) {
            char *text, *report = 0;
            unsigned long tlen = 0;
            if (dot) *(char *)dot = 0;
            text = p88_convert(buf, (unsigned long)sz, &tlen, &report);
            free(report);
            if (!text) continue;
            snprintf(path, sizeof(path), "%s/%.15s.BAS", e->workdir, base);
            fp = fopen(path, "wb");
            if (fp) { fwrite(text, 1, tlen, fp); fclose(fp); }
            free(text);
            if (!e->bootfile[0] || strcmp(base, "MENU") == 0)
                snprintf(e->bootfile, sizeof(e->bootfile), "%s.BAS", base);
        } else {
            snprintf(path, sizeof(path), "%s/%.15s", e->workdir, files[i].name);
            fp = fopen(path, "wb");
            if (fp) { fwrite(buf, 1, (size_t)sz, fp); fclose(fp); }
        }
    }
    return e->bootfile[0] ? 0 : -1;
}

int p8_boot(P8Engine *e)
{
    if (materialize(e) != 0) return -1;
    if (chdir(e->workdir) != 0) return -1;
    return 0;
}

static void *engine_thread(void *arg)
{
    P8Engine *e = (P8Engine *)arg;
    interp_init();
    hal_init(0);
    scr_init();
    g_init();
    exit_after_run = 1;
    ok_needed = 0;
    if (setjmp(p8_bail_jmp) == 0) {
        p8_bail_armed = 1;
        if (!prog_load(e->bootfile, 0)) {
            p8_bail_armed = 0;
            e->running = 0;
            return 0;
        }
        exec_direct_pending_set("RUN");
        top_level();            /* leaves via hal_term -> p8_bail */
    }
    p8_bail_armed = 0;
    e->running = 0;
    return 0;
}

void p8_start(P8Engine *e)
{
    if (e->started) return;
    e->running = 1;
    e->started = 1;
    clock_gettime(CLOCK_MONOTONIC, &e->t0);
    pthread_create(&e->th, 0, engine_thread, e);
}

void p8_stop(P8Engine *e)
{
    if (!e->started) return;
    halp8_request_stop();
    pthread_join(e->th, 0);
    e->started = 0;
    e->running = 0;
}

int p8_running(P8Engine *e) { return e->running; }

void p8_push_key(P8Engine *e, int code) { (void)e; halp8_push_key(code); }
void p8_set_matrix(P8Engine *e, int idx, unsigned char v)
{
    (void)e;
    halp8_set_matrix(idx, v);
}

void p8_frame(P8Engine *e, unsigned char *rgba)
{
    const unsigned char *g = halp8_gfx(halp8_gfx_display_page());
    int x, y;
    (void)e;
    for (y = 0; y < 400; y++) {
        for (x = 0; x < 640; x++) {
            int idx = g[y * 640 + x], r, gg, b;
            unsigned char *p = rgba + (y * 640 + x) * 4;
            halp8_palette(idx, &r, &gg, &b);
            p[0] = (unsigned char)r;
            p[1] = (unsigned char)gg;
            p[2] = (unsigned char)b;
            p[3] = 255;
        }
    }
}

const unsigned short *p8_text_codes(P8Engine *e) { (void)e; return halp8_text_codes(); }
const unsigned char  *p8_text_attrs(P8Engine *e) { (void)e; return halp8_text_attrs(); }
void p8_cursor(P8Engine *e, int *x, int *y, int *visible)
{
    (void)e;
    halp8_cursor(x, y, visible);
}
