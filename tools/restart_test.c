/* restart_test.c - two-boot regression test for the embedded engine:
 * the host application creates a fresh P8Engine per game launch in
 * ONE process, so a second create/boot/start after stop/destroy must
 * behave like the first (docs/APPROXIMATIONS.md item 22).
 *
 *   restart_test <disk.d88> <seconds-per-boot>
 *
 * Each boot prints three lines, prefixed boot1: / boot2: so a script
 * can compare them:
 *   bootN: t=3 running=R audio_frames=A
 *   bootN: end running=R audio_frames=A
 *   bootN: frame_hash=HHHHHHHH nonzero_pixels=P
 * Exit status is nonzero when boot2 is not running at t=3 or drained
 * no audio at all (the one-shot engine's symptom: the stale stop
 * request killed the second session at its first poll).
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "engine.h"

struct boot_result {
    int running3;       /* p8_running at t=3 */
    int running_end;
    long audio;         /* audio frames drained over the whole boot */
};

static int run_boot(const char *disk, int secs, int no, struct boot_result *r)
{
    static short drain[44100 * 2 * 2];          /* the ring holds 2 s */
    static unsigned char rgba[640 * 400 * 4];
    P8Engine *e;
    unsigned long h = 2166136261UL;             /* FNV-1a, 32 bit */
    long nonzero = 0;
    int i;
    r->running3 = 0; r->running_end = 0; r->audio = 0;
    e = p8_create(disk);
    if (!e) { fprintf(stderr, "boot%d: create failed\n", no); return -1; }
    if (p8_boot(e) != 0) {
        fprintf(stderr, "boot%d: boot failed\n", no);
        p8_destroy(e);
        return -1;
    }
    p8_start(e);
    for (i = 1; i <= secs; i++) {
        int n;
        sleep(1);
        n = p8_audio_read(e, drain, (int)(sizeof(drain) / 4));
        if (n > 0) r->audio += n;
        if (i == 3) {
            r->running3 = p8_running(e);
            printf("boot%d: t=3 running=%d audio_frames=%ld\n",
                   no, r->running3, r->audio);
        }
    }
    r->running_end = p8_running(e);
    printf("boot%d: end running=%d audio_frames=%ld\n",
           no, r->running_end, r->audio);
    /* stop before sampling: the join synchronizes with the engine
     * thread, and the frame buffers survive the stop, so the hash is
     * taken from a quiescent frame (a pause request + sleep would
     * only be honoured at the interpreter's next poll) */
    p8_stop(e);
    p8_frame(e, rgba);
    for (i = 0; i < 640 * 400 * 4; i++) {
        h ^= rgba[i];
        h = (h * 16777619UL) & 0xFFFFFFFFUL;
    }
    for (i = 0; i < 640 * 400; i++)
        if (rgba[i * 4] | rgba[i * 4 + 1] | rgba[i * 4 + 2]) nonzero++;
    printf("boot%d: frame_hash=%08lx nonzero_pixels=%ld\n", no, h, nonzero);
    fflush(stdout);
    p8_destroy(e);
    return 0;
}

int main(int argc, char **argv)
{
    struct boot_result b1, b2;
    int secs, rc = 0;
    if (argc < 3) {
        fprintf(stderr, "usage: restart_test <disk.d88> <seconds-per-boot>\n");
        return 2;
    }
    secs = atoi(argv[2]);
    if (secs < 3) secs = 3;                     /* the t=3 sample must exist */
    /* p8_boot chdirs into the session's scratch dir, so a relative
     * disk path would resolve inside boot1's scratch on the second
     * boot - make it absolute once, up front */
    {
        char *abs_disk = realpath(argv[1], NULL);
        if (!abs_disk) { fprintf(stderr, "cannot resolve %s\n", argv[1]); return 2; }
        if (run_boot(abs_disk, secs, 1, &b1) != 0) return 1;
        if (run_boot(abs_disk, secs, 2, &b2) != 0) return 1;
        free(abs_disk);
    }
    if (!b2.running3) { fprintf(stderr, "FAIL: boot2 not running at t=3\n"); rc = 1; }
    if (b2.audio <= 0) { fprintf(stderr, "FAIL: boot2 produced no audio\n"); rc = 1; }
    return rc;
}
