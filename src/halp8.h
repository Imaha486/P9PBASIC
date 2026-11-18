/* halp8.h - accessors for the embedded HAL (halp8.c), used by the
 * engine to expose the screen buffers to the host application. */
#ifndef HALP8_H
#define HALP8_H

#ifdef __cplusplus
extern "C" {
#endif

/* text screen: 80x25 cells of (code, attr); graphics: 2 pages of
 * 640x400 palette indices. Pointers stay valid for the process
 * lifetime; contents change as the guest runs. */
const unsigned short *halp8_text_codes(void);
const unsigned char  *halp8_text_attrs(void);
const unsigned char  *halp8_gfx(int page);
int  halp8_gfx_display_page(void);
int  halp8_gfx_visible(void);
int  halp8_text_visible(void);
int  halp8_width40(void);
void halp8_palette(int idx, int *r, int *g, int *b);   /* 0..255 */
void halp8_cursor(int *x, int *y, int *visible);

/* input injection (thread-safe) */
void halp8_push_key(int code);
void halp8_set_matrix(int idx, unsigned char v);
void halp8_request_stop(void);
int  halp8_stop_requested(void);
void halp8_set_paused(int paused);
int  halp8_paused(void);

#ifdef __cplusplus
}
#endif

#endif
