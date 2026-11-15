/* SNES tile cache: the libdragon half (tile_cache.h). */
#include "tile_cache.h"

#include <libdragon.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

DEFINE_RSP_UCODE(rsp_tile);

typedef struct __attribute__((packed, aligned(16))) {
  uint32_t vram, prev, c2, c4, c8;
  uint16_t first, nblocks;
  uint32_t pad, result;
} TileJob;
_Static_assert(sizeof(TileJob) == TCJ_SIZE, "tile job layout");

typedef struct __attribute__((packed, aligned(16))) {
  uint32_t cgram, dst;
  uint16_t bright, kind, base_lo, base_hi;
} TlutJob;
_Static_assert(sizeof(TlutJob) == TLJ_SIZE, "tlut job layout");

enum { kJobs = 4 };   /* jobs in flight (ring) */

static uint32_t s_ovl;
static uint8_t *s_prev, *s_c2, *s_c4, *s_c8;
static TileJob *s_jobs;
static TlutJob *s_tjobs;
static unsigned s_job, s_tjob;

static uint8_t *alloc_zero(size_t n) {
  uint8_t *p = memalign(16, n);
  if (!p) abort();
  memset(p, 0, n);
  data_cache_hit_writeback_invalidate(p, n);
  return p;
}

void n64_tile_cache_init(unsigned formats) {
  s_ovl = rspq_overlay_register(&rsp_tile);
  s_prev = alloc_zero(0x10000);
  if (formats & TC_FMT_2BPP) s_c2 = alloc_zero((size_t)TC_C2_TILES * TC_CI4_TILE);
  if (formats & TC_FMT_4BPP) s_c4 = alloc_zero((size_t)TC_C4_TILES * TC_CI4_TILE);
  if (formats & TC_FMT_8BPP) s_c8 = alloc_zero((size_t)TC_C8_TILES * TC_CI8_TILE);
  s_jobs = (TileJob *)alloc_zero(sizeof(TileJob) * kJobs);
  s_tjobs = (TlutJob *)alloc_zero(sizeof(TlutJob) * kJobs);
}

static uint32_t phys_or_0(const void *p) { return p ? PhysicalAddr(p) : 0; }

volatile uint32_t *n64_tile_cache_sync(const uint16_t *vram) {
  TileJob *j = UncachedAddr(&s_jobs[s_job++ % kJobs]);
  data_cache_hit_writeback(vram, 0x10000);
  j->vram = PhysicalAddr(vram);
  j->prev = PhysicalAddr(s_prev);
  j->c2 = phys_or_0(s_c2);
  j->c4 = phys_or_0(s_c4);
  j->c8 = phys_or_0(s_c8);
  j->first = 0;
  j->nblocks = 0x10000 / TC_BLOCK;
  j->pad = 0;
  j->result = 0;
  rspq_write(s_ovl, 0, PhysicalAddr(j));
  return &j->result;
}

void n64_tile_cache_tlut(const uint16_t *cgram, unsigned brightness, unsigned kind,
                         unsigned base_lo, unsigned base_hi, uint16_t *dst) {
  TlutJob *j = UncachedAddr(&s_tjobs[s_tjob++ % kJobs]);
  data_cache_hit_writeback(cgram, 512);
  data_cache_hit_writeback_invalidate(dst, 512);
  j->cgram = PhysicalAddr(cgram);
  j->dst = PhysicalAddr(dst);
  j->bright = (uint16_t)brightness;
  j->kind = (uint16_t)kind;
  j->base_lo = (uint16_t)base_lo;
  j->base_hi = (uint16_t)base_hi;
  rspq_write(s_ovl, 1, PhysicalAddr(j));
}

const uint8_t *n64_tile_cache_c2(void) { return s_c2; }
const uint8_t *n64_tile_cache_c4(void) { return s_c4; }
const uint8_t *n64_tile_cache_c8(void) { return s_c8; }
