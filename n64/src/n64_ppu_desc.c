/* The frame description (include/n64_ppu_desc.h): bands of identical PPU
 * registers and CGRAM, built by Green's raster replay one line at a time
 * (draw_frame_rdp in n64_host.c). */
#include "n64_ppu_desc.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "snes/ppu.h"

_Static_assert(offsetof(N64PpuRegs, cgwsel) == offsetof(Ppu, cgwsel),
               "N64PpuRegs must match the head of struct Ppu");
_Static_assert(sizeof(N64PpuRegs) <= offsetof(Ppu, vramPointer),
               "N64PpuRegs must be the PPU's register block");

static N64PpuDesc s_desc;
static uint32_t s_vram_count, s_oam_count, s_cgram_count;
static bool s_open;

const N64PpuDesc *n64_ppu_desc(void) { return &s_desc; }
int n64_ppu_desc_bands(void) { return s_desc.nbands; }

static void new_cgram(const Ppu *ppu) {
  s_cgram_count = ppu->cgramWriteCount;
  if (s_desc.ncgram == N64PPU_DESC_MAX_CGRAMS) {
    s_desc.flags |= N64PPU_DESC_CGRAM_OVERFLOW;
    return;   /* the last version stands in (the check build reports it) */
  }
  memcpy(s_desc.cgram[s_desc.ncgram++], ppu->cgram, sizeof s_desc.cgram[0]);
}

#ifdef N64PPU_DESC_CHECK
static void check_begin(const Ppu *ppu);
static void check_end(const Ppu *ppu);
#endif

void n64_ppu_desc_begin(const Ppu *ppu) {
  s_desc.nbands = 0;
  s_desc.ncgram = 0;
  s_desc.flags = 0;
  s_desc.vram = ppu->vram;
  s_desc.oam = ppu->oam;
  s_desc.high_oam = ppu->highOam;
  s_vram_count = ppu->vramWriteCount;
  s_oam_count = ppu->oamWriteCount;
  new_cgram(ppu);
  s_open = true;
#ifdef N64PPU_DESC_CHECK
  check_begin(ppu);
#endif
}

/* Line `line` (1-based) with the registers the replay has just produced.
 * Returns true when it joined the previous line's band: same registers and
 * CGRAM version. */
bool n64_ppu_desc_line(const Ppu *ppu, int line) {
  if (!s_open) return false;
  if (ppu->cgramWriteCount != s_cgram_count) new_cgram(ppu);
  if (ppu->vramWriteCount != s_vram_count) s_desc.flags |= N64PPU_DESC_VRAM_CHANGED;
  if (ppu->oamWriteCount != s_oam_count) s_desc.flags |= N64PPU_DESC_OAM_CHANGED;
  const uint8_t cg = (uint8_t)(s_desc.ncgram - 1);
  if (s_desc.nbands) {
    N64PpuBand *b = &s_desc.band[s_desc.nbands - 1];
    if (b->y1 == line - 1 && b->cgram == cg && !memcmp(&b->regs, ppu, sizeof b->regs)) {
      b->y1 = (uint8_t)line;
      return true;
    }
  }
  if (s_desc.nbands == N64PPU_DESC_LINES) return false;   /* cannot happen: one band per line at most */
  N64PpuBand *b = &s_desc.band[s_desc.nbands++];
  b->y0 = b->y1 = (uint8_t)line;
  b->cgram = cg;
  b->pad_ = 0;
  memcpy(&b->regs, ppu, sizeof b->regs);
  return false;
}

void n64_ppu_desc_end(const Ppu *ppu) {
  if (!s_open) return;
  s_open = false;
#ifdef N64PPU_DESC_CHECK
  check_end(ppu);
#else
  (void)ppu;
#endif
}

#ifdef N64PPU_DESC_CHECK
/* Completeness check (native builds): a scratch PPU, copied from the live
 * one when the replay begins, is then fed only from the description (each
 * band's registers and CGRAM version, the frame's VRAM and OAM) and drawn by
 * the software PPU line by line; its pixels must equal the live software
 * PPU's (the host draws every line in software in this build). */
#include <stdio.h>
static Ppu *s_scratch;
static uint8_t *s_scratch_px;
static uint32_t s_frames, s_bad_frames, s_bands_total, s_cgram_max, s_cgram_multi, s_vram_mid, s_oam_mid;
uint8_t *g_n64ppu_desc_live_pixels;   /* set by the host: the live software PPU's output */
size_t g_n64ppu_desc_live_pitch;

static void check_begin(const Ppu *ppu) {
  if (!s_scratch) {
    s_scratch = malloc(sizeof *s_scratch);
    s_scratch_px = calloc(1, 256 * 4 * 240 + 4096);
  }
  memcpy(s_scratch, ppu, sizeof *s_scratch);
}

static void check_end(const Ppu *ppu) {
  (void)ppu;
  if (!g_n64ppu_desc_live_pixels) return;
  PpuBeginDrawing(s_scratch, s_scratch_px, g_n64ppu_desc_live_pitch, ppu->renderFlags);
  s_frames++;
  s_bands_total += (uint32_t)s_desc.nbands;
  if ((uint32_t)s_desc.ncgram > s_cgram_max) s_cgram_max = (uint32_t)s_desc.ncgram;
  if (s_desc.ncgram > 1) s_cgram_multi++;
  if (s_desc.flags & N64PPU_DESC_VRAM_CHANGED) s_vram_mid++;
  if (s_desc.flags & N64PPU_DESC_OAM_CHANGED) s_oam_mid++;
  int bad = 0, first = -1;
  for (int k = 0; k < s_desc.nbands; ++k) {
    const N64PpuBand *b = &s_desc.band[k];
    for (int line = b->y0; line <= b->y1; ++line) {
#ifdef N64PPU_DESC_CHECK_BREAK   /* the check must fail: every band drawn with the first band's registers */
      memcpy(s_scratch, &s_desc.band[0].regs, sizeof b->regs);
#else
      memcpy(s_scratch, &b->regs, sizeof b->regs);
#endif
#ifndef N64PPU_DESC_CHECK_BREAK
      memcpy(s_scratch->cgram, s_desc.cgram[b->cgram], sizeof s_scratch->cgram);
#endif
      ppu_runLine(s_scratch, line);
      const size_t off = (size_t)(line - 1) * g_n64ppu_desc_live_pitch;
      if (memcmp(s_scratch_px + off, g_n64ppu_desc_live_pixels + off, 256 * 4)) {
        if (first < 0) first = line;
        bad++;
      }
    }
  }
  if (bad) {
    s_bad_frames++;
    if (s_bad_frames <= 10)
      fprintf(stderr, "[ppudesc] frame %lu: %d lines differ (first %d), %d bands, %d cgram, flags %lx\n",
              (unsigned long)s_frames, bad, first, s_desc.nbands, s_desc.ncgram,
              (unsigned long)s_desc.flags);
  }
  if (s_frames % 300 == 0)
    fprintf(stderr, "[ppudesc] %lu frames: %lu differ; %.1f bands a frame; CGRAM versions max %lu "
            "(frames with more than one: %lu); VRAM changed mid-frame %lu, OAM %lu\n",
            (unsigned long)s_frames, (unsigned long)s_bad_frames, (double)s_bands_total / s_frames,
            (unsigned long)s_cgram_max, (unsigned long)s_cgram_multi, (unsigned long)s_vram_mid,
            (unsigned long)s_oam_mid);
}
#endif
