/* The frame description: what Blue's frame leaves for the RSP to draw
 * (docs/n64/PPU_DESIGN.md, work plan step 1).
 *
 * Green's replay of the raster journal (journal + HDMA, draw_frame_rdp)
 * produces each visible line's PPU registers. The description groups lines
 * whose registers and CGRAM are identical into bands, and records the
 * memories they read: one VRAM and one OAM image per frame (the game cannot
 * write them while the picture is drawn; a change mid-frame is flagged) and
 * every CGRAM version the frame uses (HDMA does change CGRAM between lines).
 *
 * A band's registers are the PPU's register block byte for byte (the same
 * X-macro list as the head of struct Ppu, ppu_register_fields.inc), so the
 * description carries the PPU's own semantics; turning them into tiles,
 * sprites and RDP commands is the RSP's job. The check build
 * (N64PPU_DESC_CHECK, native) proves the description complete: the software
 * PPU, fed only from it, must draw the same pixels.
 *
 * This header includes neither libdragon nor the SNES runtime. */
#ifndef SNESRECOMP_N64_PPU_DESC_H
#define SNESRECOMP_N64_PPU_DESC_H

#include <stdbool.h>
#include <stdint.h>

/* The PPU register block (the head of struct Ppu). */
typedef struct N64PpuRegs {
#define SNESRECOMP_PPU_REGISTER(type, name, array) type name array;
#include "ppu_register_fields.inc"
#undef SNESRECOMP_PPU_REGISTER
} N64PpuRegs;

enum {
  N64PPU_DESC_LINES = 224,
  N64PPU_DESC_MAX_CGRAMS = 32,   /* CGRAM versions per frame */
};

/* Lines [y0, y1] (visible lines, 1-based) with identical registers and
 * CGRAM version. */
typedef struct N64PpuBand {
  uint8_t y0, y1;
  uint8_t cgram;                 /* index into N64PpuDesc.cgram */
  uint8_t pad_;
  N64PpuRegs regs;
} N64PpuBand;

/* N64PpuDesc.flags */
enum {
  N64PPU_DESC_VRAM_CHANGED = 1u << 0,    /* VRAM written while the frame was drawn */
  N64PPU_DESC_OAM_CHANGED = 1u << 1,     /* OAM written while the frame was drawn */
  N64PPU_DESC_CGRAM_OVERFLOW = 1u << 2,  /* more CGRAM versions than N64PPU_DESC_MAX_CGRAMS */
};

typedef struct N64PpuDesc {
  int nbands;
  N64PpuBand band[N64PPU_DESC_LINES];
  int ncgram;
  uint16_t cgram[N64PPU_DESC_MAX_CGRAMS][256];
  /* The frame's VRAM and OAM images (the PPU's own arrays here; the RSP
   * side reads its snapshot of them). */
  const uint16_t *vram;
  const uint16_t *oam;
  const uint8_t *high_oam;
  uint32_t flags;
} N64PpuDesc;

#endif
