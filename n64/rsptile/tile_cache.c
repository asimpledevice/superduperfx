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
  uint32_t z2, z4, pad2[2];
} TileJob;
_Static_assert(sizeof(TileJob) == TCJ_SIZE, "tile job layout");

typedef struct __attribute__((packed, aligned(16))) {
  uint32_t cgram, dst;
  uint16_t bright, kind, base_lo, base_hi;
} TlutJob;
_Static_assert(sizeof(TlutJob) == TLJ_SIZE, "tlut job layout");

enum { kJobs = 64 };   /* jobs in flight (ring): a frame queues at most kMaxRuns + TLUTs */

static uint32_t s_ovl;
static uint8_t *s_prev, *s_c2, *s_c4, *s_c8, *s_z2, *s_z4;
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
  /* Empty-tile bitmaps: everything starts empty (zero VRAM). */
  s_z2 = alloc_zero(512);
  s_z4 = alloc_zero(256);
  memset(s_z2, 0xff, 512);
  memset(s_z4, 0xff, 256);
  data_cache_hit_writeback_invalidate(s_z2, 512);
  data_cache_hit_writeback_invalidate(s_z4, 256);
  s_jobs = (TileJob *)alloc_zero(sizeof(TileJob) * kJobs);
  s_tjobs = (TlutJob *)alloc_zero(sizeof(TlutJob) * kJobs);
}

static uint32_t phys_or_0(const void *p) { return p ? PhysicalAddr(p) : 0; }

static volatile uint32_t *sync_range(const uint16_t *vram, unsigned first, unsigned nblocks) {
  TileJob *j = UncachedAddr(&s_jobs[s_job++ % kJobs]);
  data_cache_hit_writeback((const uint8_t *)vram + first * TC_BLOCK, nblocks * TC_BLOCK);
  j->vram = PhysicalAddr(vram);
  j->prev = PhysicalAddr(s_prev);
  j->c2 = phys_or_0(s_c2);
  j->c4 = phys_or_0(s_c4);
  j->c8 = phys_or_0(s_c8);
  j->first = (uint16_t)first;
  j->nblocks = (uint16_t)nblocks;
  j->pad = 0;
  j->result = 0;
  j->z2 = PhysicalAddr(s_z2);
  j->z4 = PhysicalAddr(s_z4);
  rspq_write(s_ovl, 0, PhysicalAddr(j));
  return &j->result;
}

volatile uint32_t *n64_tile_cache_sync(const uint16_t *vram) {
  return sync_range(vram, 0, 0x10000 / TC_BLOCK);
}

unsigned n64_tile_cache_sync_dirty(const uint16_t *vram, const uint32_t dirty[8]) {
  /* Runs of dirty blocks, joined across gaps of up to kGap clean blocks (a
   * job costs more than comparing a few clean blocks); past kMaxRuns runs,
   * the last job covers everything to the last dirty block. */
  enum { kBlocks = 0x10000 / TC_BLOCK, kGap = 2, kMaxRuns = 16 };
  unsigned runs = 0, total = 0;
  int b = 0;
  while (b < kBlocks) {
    if (!((dirty[b >> 5] >> (b & 31)) & 1)) { ++b; continue; }
    const int first = b;
    int last = b, clean = 0;
    for (++b; b < kBlocks; ++b) {
      if ((dirty[b >> 5] >> (b & 31)) & 1) { last = b; clean = 0; }
      else if (++clean > kGap && runs + 1 < kMaxRuns) break;
    }
    sync_range(vram, (unsigned)first, (unsigned)(last + 1 - first));
    total += (unsigned)(last + 1 - first);
    runs++;
  }
  return total;
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
const uint8_t *n64_tile_cache_empty2(void) { return s_z2; }
const uint8_t *n64_tile_cache_empty4(void) { return s_z4; }
