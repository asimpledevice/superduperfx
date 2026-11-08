/* RDP backend, runtime half (see n64_ppu_rdp.h). Only for files that also
 * see the SNES runtime headers. */
#ifndef SNESRECOMP_N64_PPU_BUILD_H
#define SNESRECOMP_N64_PPU_BUILD_H

#include <stdbool.h>
#include <stdint.h>

#include "n64_ppu_rdp.h"

struct Ppu;

/* One frame description, with its plane buffers. */
N64PpuFrame *n64_ppu_build_frame_alloc(void);

/* Start a frame. sw_rows: the software PPU's 256-wide XRGB8888 output, used
 * for lines that fall back (and for comparison). */
void n64_ppu_build_begin(N64PpuFrame *frame, const uint32_t *sw_rows);

/* Expand visible line 1..224 from the PPU's current (per-line) state.
 * Returns false when the line needs the software PPU. */
bool n64_ppu_build_line(const struct Ppu *ppu, int line);

/* Finish the frame: expand background runs still deferred. */
void n64_ppu_build_end(void);

#endif
