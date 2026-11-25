/* Two-screen colour math on the RSP (rsp_cmp.S): the libdragon half. */
#ifndef N64_CMP_RSP_H
#define N64_CMP_RSP_H

#include <stdbool.h>
#include <stdint.h>

#include "n64_ppu_rdp.h"

/* The caller initialises rspq first. */
void n64_cmprsp_init(void);
void n64_cmprsp_frame_begin(void);
/* Queue rows [a, b) of a finished frame, a band of N64PPU_LINE_COMPOSE
 * lines, to be written as RGBA5551 into out (row r at out + 256 * r).
 * Returns false when the RSP cannot take the band (direct colour, or the
 * frame's descriptors are used up): the caller composes it on the CPU. */
bool n64_cmprsp_band(const N64PpuFrame *f, int a, int b, uint16_t *out);

#endif
