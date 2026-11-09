/* SNES BG expansion on the RSP: the libdragon half. See bg_rsp.h. */
#include "bg_rsp.h"

#include <libdragon.h>
#include <malloc.h>
#include <stdlib.h>

#include "bg_rsp_layout.h"

DEFINE_RSP_UCODE(rsp_bg);

typedef struct __attribute__((packed, aligned(16))) {
  uint32_t vram, dst, flags;
  uint16_t nrows, y, hs, tileadr;
  uint8_t xsc, bpp, pal, pad;
  uint8_t pad2[8];
} BgJob;
_Static_assert(sizeof(BgJob) == BGJ_SIZE, "job layout");

enum { kMaxJobs = 64 };

static uint32_t s_ovl;
static BgJob *s_jobs;       /* kMaxJobs blocks */
static uint8_t *s_flags;    /* kMaxJobs * BGJ_MAX_ROWS, written by the RSP */
static int s_njobs;
static uint32_t s_vram_count;
static bool s_vram_valid;

void n64_bgrsp_init(void) {
  s_ovl = rspq_overlay_register(&rsp_bg);
  s_jobs = memalign(16, sizeof(BgJob) * kMaxJobs);
  s_flags = memalign(16, BGJ_MAX_ROWS * kMaxJobs);
  if (!s_jobs || !s_flags) abort();
  /* The RSP writes the flags: no dirty CPU line may cover them. */
  data_cache_hit_writeback_invalidate(s_flags, BGJ_MAX_ROWS * kMaxJobs);
}

void n64_bgrsp_frame_begin(void) { s_njobs = 0; }

void n64_bgrsp_sync_vram(const uint16_t *vram, uint32_t vram_count) {
  if (s_vram_valid && vram_count == s_vram_count) return;
  if (s_njobs) rspq_wait();   /* queued jobs read the old contents */
  data_cache_hit_writeback(vram, 0x10000);
  s_vram_count = vram_count;
  s_vram_valid = true;
}

void n64_bgrsp_cpu_wrote(void *p, unsigned n) { data_cache_hit_writeback_invalidate(p, n); }

int n64_bgrsp_submit(const uint16_t *vram, uint8_t *dst, int nrows, uint16_t y,
                     uint16_t hs, uint16_t tileadr, uint8_t xsc, uint8_t bpp, uint8_t pal) {
  if (s_njobs == kMaxJobs) return -1;
  BgJob *j = &s_jobs[s_njobs];
  j->vram = PhysicalAddr(vram);
  j->dst = PhysicalAddr(dst);
  j->flags = PhysicalAddr(s_flags + (size_t)s_njobs * BGJ_MAX_ROWS);
  j->nrows = (uint16_t)nrows;
  j->y = y;
  j->hs = hs;
  j->tileadr = tileadr;
  j->xsc = xsc;
  j->bpp = bpp;
  j->pal = pal;
  data_cache_hit_writeback(j, sizeof *j);
  rspq_write(s_ovl, 0, PhysicalAddr(j));
  return s_njobs++;
}

void n64_bgrsp_wait(void) {
  rspq_wait();
  data_cache_hit_invalidate(s_flags, (size_t)BGJ_MAX_ROWS * (size_t)(s_njobs ? s_njobs : 1));
}

const uint8_t *n64_bgrsp_flags(int job) { return s_flags + (size_t)job * BGJ_MAX_ROWS; }
