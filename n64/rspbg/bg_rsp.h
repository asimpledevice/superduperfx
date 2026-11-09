/* SNES BG expansion on the RSP (rsp_bg.S): the libdragon half. */
#ifndef N64_BG_RSP_H
#define N64_BG_RSP_H

#include <stdbool.h>
#include <stdint.h>

/* The caller initialises rspq first (rspq_init, or rdpq_init). */
void n64_bgrsp_init(void);
/* Start a frame's jobs. */
void n64_bgrsp_frame_begin(void);
/* Make VRAM visible to the RSP if it changed since the last call (by its
 * write counter); waits for this frame's queued jobs first. */
void n64_bgrsp_sync_vram(const uint16_t *vram, uint32_t vram_count);
/* The CPU wrote [p, p + n) of memory the RSP also writes (plane rows):
 * write it back and drop it from the cache, so no dirty line can later
 * overwrite what the RSP stores there. */
void n64_bgrsp_cpu_wrote(void *p, unsigned n);
/* Queue one run: nrows rows starting at dst (a plane row + N64PPU_PAD).
 * Returns the job index (for n64_bgrsp_flags) or -1 when the frame's jobs
 * are used up (the caller expands the run itself). */
int n64_bgrsp_submit(const uint16_t *vram, uint8_t *dst, int nrows, uint16_t y,
                     uint16_t hs, uint16_t tileadr, uint8_t xsc, uint8_t bpp, uint8_t pal);
/* Wait for the frame's jobs; then n64_bgrsp_flags(job)[i] is row i's flags
 * (bit 0: a low-priority pixel is non-zero, bit 1: a high-priority one). */
void n64_bgrsp_wait(void);
const uint8_t *n64_bgrsp_flags(int job);

#endif
