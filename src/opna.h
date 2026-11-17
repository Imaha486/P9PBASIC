/* opna.h - OPNA (YM2608) sound rendering for the host build (ymfm).
 *   The vessel's port model forwards every OPNA register write to
 *   opna_write(); opna_advance() is driven by emulated CPU cycles and
 *   renders the corresponding span of audio. The headless harness has
 *   no speakers, so rendering happens only when the P8PWAV environment
 *   variable names an output WAV file; otherwise both calls are
 *   cheap no-ops.
 */
#ifndef OPNA_H
#define OPNA_H

#ifdef __cplusplus
extern "C" {
#endif

void opna_write(int bank, int addr, int value);
void opna_advance(long cpu_cycles);
/* Write one byte into the OPNA's ADPCM-A external memory (the Sound
 * Board II's writable sample RAM that FFDE service 1 loads into). */
void opna_adpcm_a_write(unsigned long addr, int value);

/* Embedded use: receive rendered stereo frames (interleaved s16) as
 * they are produced; passing NULL removes the sink. Rendering runs
 * when P8PWAV names a WAV file OR a sink is registered. */
typedef void (*opna_sink_fn)(const short *frames, int count, void *ctx);
void opna_set_sink(opna_sink_fn fn, void *ctx);

/* Reset the chip, both sample RAMs and the render position; call
 * before a fresh boot. */
void opna_reset(void);

#ifdef __cplusplus
}
#endif

#endif
