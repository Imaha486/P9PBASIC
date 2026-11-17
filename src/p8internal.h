/* p8internal.h - glue shared by the embedded engine (engine.c) and
 * the embedded HAL (halp8.c). Not part of the public engine API.
 *
 * The interpreter ends a run with hal_term() followed by exit(0)
 * (exec.c, when exit_after_run is set). The engine arms a longjmp
 * target instead: hal_term() in halp8.c jumps back into the engine
 * thread, which then reports the stopped state. The same target is
 * used to bail out of input waits when a stop is requested. */
#ifndef P8INTERNAL_H
#define P8INTERNAL_H

#include <setjmp.h>

extern jmp_buf p8_bail_jmp;
extern volatile int p8_bail_armed;

/* Jump back to the engine thread if armed; otherwise return. */
void p8_bail(void);

/* Pacing clocks provided by engine.c (single engine instance):
 * rendered-audio time and wall time since p8_start, in seconds.
 * halp8's hal_poll throttles the interpreter to the audio clock so
 * playback runs at real speed inside a host application. */
double p8_audio_seconds(void);
double p8_wall_seconds(void);

#endif
