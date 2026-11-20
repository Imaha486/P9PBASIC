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
int  opna_status_hi(void);   /* extended status (ADPCM-B flags) */
void opna_advance(long cpu_cycles);
/* Write / read one byte of the OPNA's ADPCM-B sample DRAM (256 KiB,
 * the Sound Board II's only writable sample memory). FFDE service 1
 * loads the PCM data block here and PMD's PCM part plays it back
 * through the bank-1 ADPCM-B registers. The read is for tests and
 * diagnostics; out-of-range addresses are ignored / read as 0. */
void opna_adpcm_b_write(unsigned long addr, int value);
int  opna_adpcm_b_read(unsigned long addr);
/* Write / read one byte of the ADPCM-A (rhythm) memory. On the
 * YM2608 this is a ROM inside the chip, not loadable by the guest;
 * nothing calls the write today. It is kept as the entry point for
 * a future host-side rhythm ROM supply (until then the rhythm
 * channels read zeros). */
void opna_adpcm_a_write(unsigned long addr, int value);
int  opna_adpcm_a_read(unsigned long addr);

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
