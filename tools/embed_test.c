/* embed_test.c - headless exercise of the embedded engine API:
 * boots a D88 through engine.h only, feeds a key script, and dumps
 * the text screen, a graphics PPM and the drained audio as WAV.
 *
 *   embed_test <disk.d88> <seconds> [keys]
 * keys use the halhost escapes (\r \e \u \d \l \R \1..\0 \w=1s wait)
 * for convenience; \w waits one second of wall time here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "engine.h"
#include "hal.h"        /* KC_* */

static void dump_text(P8Engine *e)
{
    const unsigned short *tv = p8_text_codes(e);
    int x, y;
    printf("----- SCREEN -----\n");
    for (y = 0; y < 25; y++) {
        char line[200];
        int o = 0;
        for (x = 0; x < 80; x++) {
            unsigned short v = tv[y * 80 + x];
            if ((v & 0xFF00) && !(v & 0x80)) {
                unsigned c1 = (v & 0x7F) + 0x20, c2 = (v >> 8) & 0x7F, s1, s2;
                s1 = ((c1 + 1) >> 1) + (c1 <= 0x5E ? 0x70 : 0xB0);
                s2 = c2 + ((c1 & 1) ? (c2 >= 0x60 ? 0x20 : 0x1F) : 0x7E);
                line[o++] = (char)s1; line[o++] = (char)s2;
            } else if (v & 0xFF00) {
            } else line[o++] = (v >= 0x20 && v < 0x7F) ? (char)v : (v == 0 ? ' ' : '?');
        }
        while (o > 0 && line[o - 1] == ' ') o--;
        line[o] = 0;
        printf("%02d|%s\n", y, line);
    }
}

static void dump_ppm(P8Engine *e, const char *path)
{
    static unsigned char rgba[640 * 400 * 4];
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return;
    p8_frame(e, rgba);
    fprintf(f, "P6 640 400 255\n");
    for (i = 0; i < 640 * 400; i++) fwrite(rgba + i * 4, 1, 3, f);
    fclose(f);
}

/* momentary PC-88 matrix key press (300 ms), for games that poll
 * INP() rows instead of reading the key queue */
static unsigned char shadow_rows[16];
static void matrix_press(P8Engine *e, int row, int bit)
{
    shadow_rows[row] &= (unsigned char)~(1u << bit);
    p8_set_matrix(e, row, shadow_rows[row]);
    usleep(300000);
    shadow_rows[row] |= (unsigned char)(1u << bit);
    p8_set_matrix(e, row, shadow_rows[row]);
}

static void wav_write(const char *path, const short *pcm, long frames)
{
    FILE *f = fopen(path, "wb");
    unsigned char h[44];
    unsigned long data = (unsigned long)frames * 4;
    if (!f) return;
    memset(h, 0, 44);
    memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); memcpy(h + 36, "data", 4);
    h[4] = (36 + data) & 0xFF; h[5] = ((36 + data) >> 8) & 0xFF;
    h[6] = ((36 + data) >> 16) & 0xFF; h[7] = ((36 + data) >> 24) & 0xFF;
    h[16] = 16; h[20] = 1; h[22] = 2;
    h[24] = 44100 & 0xFF; h[25] = (44100 >> 8) & 0xFF;
    h[28] = (44100 * 4) & 0xFF; h[29] = ((44100 * 4) >> 8) & 0xFF;
    h[30] = ((44100 * 4) >> 16) & 0xFF; h[31] = ((44100 * 4) >> 24) & 0xFF;
    h[32] = 4; h[34] = 16;
    h[40] = data & 0xFF; h[41] = (data >> 8) & 0xFF;
    h[42] = (data >> 16) & 0xFF; h[43] = (data >> 24) & 0xFF;
    fwrite(h, 1, 44, f);
    fwrite(pcm, 2, (size_t)frames * 2, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    P8Engine *e;
    const char *keys;
    int secs, i;
    static short drain[44100 * 120];
    long got = 0;
    if (argc < 3) {
        fprintf(stderr, "usage: embed_test <disk.d88> <seconds> [keys]\n");
        return 2;
    }
    secs = atoi(argv[2]);
    keys = argc > 3 ? argv[3] : "";
    memset(shadow_rows, 0xFF, sizeof(shadow_rows));
    e = p8_create(argv[1]);
    if (!e) { fprintf(stderr, "create failed\n"); return 1; }
    if (p8_boot(e) != 0) { fprintf(stderr, "boot failed\n"); return 1; }
    p8_start(e);
    /* feed the key script in real time, draining audio as we go */
    for (i = 0; keys[i]; i++) {
        char c = keys[i];
        int n = p8_audio_read(e, drain + got * 2,
                              (long)sizeof(drain) / 4 - got);
        if (n > 0) got += n;
        if (c != '\\') { p8_push_key(e, (unsigned char)c); continue; }
        c = keys[++i];
        switch (c) {
        case 'r': case 'n': p8_push_key(e, 13); break;
        case 'e': p8_push_key(e, 27); break;
        case 'u': p8_push_key(e, 0x1E); break;
        case 'd': p8_push_key(e, 0x1F); break;
        case 'l': p8_push_key(e, 0x1D); break;
        case 'R': p8_push_key(e, 0x1C); break;
        case 'w': sleep(1); break;
        case 'D': matrix_press(e, 0xA, 1); break;   /* cursor down */
        case 'U': matrix_press(e, 0x8, 1); break;   /* cursor up */
        case 'S': matrix_press(e, 0x9, 6); break;   /* SPACE */
        case 'E': matrix_press(e, 0x1, 7); break;   /* RETURN */
        default:
            if (c >= '1' && c <= '9') p8_push_key(e, KC_F1 + (c - '1'));
            else if (c == '0') p8_push_key(e, KC_F1 + 9);
            else p8_push_key(e, c);
            break;
        }
    }
    for (i = 0; i < secs; i++) {
        int n;
        sleep(1);
        n = p8_audio_read(e, drain + got * 2, (long)sizeof(drain) / 4 - got);
        if (n > 0) got += n;
        if (!p8_running(e)) break;
    }
    printf("running=%d audio_frames=%ld dropped=%lu\n",
           p8_running(e), got, p8_audio_dropped(e));
    dump_text(e);
    dump_ppm(e, "embed_gfx.ppm");
    wav_write("embed_audio.wav", drain, got);
    p8_stop(e);
    p8_destroy(e);
    return 0;
}
