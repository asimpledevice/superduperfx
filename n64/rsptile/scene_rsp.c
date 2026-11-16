/* Scene builder on the RSP: the libdragon half (rsp_scene.S). */
#include "scene_rsp.h"

#include <libdragon.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "scene_layout.h"

DEFINE_RSP_UCODE(rsp_scene);

typedef struct __attribute__((packed, aligned(16))) {
  uint32_t vram, cache;
  uint16_t hs, vs;
  uint8_t xsc, wsh;
  uint16_t tileadr;
  uint8_t r0, r1, prio, pad;
  uint16_t sx0, sx1, ox, oy;
  uint32_t result;
} SceneJob;
_Static_assert(sizeof(SceneJob) == SCJ_SIZE, "scene job layout");

enum { kJobs = 1024 };   /* ring: far more than a frame's passes */

static uint32_t s_ovl;
static SceneJob *s_jobs;
static unsigned s_next;

void n64_scene_rsp_init(void) {
  s_ovl = rspq_overlay_register(&rsp_scene);
  s_jobs = memalign(16, sizeof(SceneJob) * kJobs);
  if (!s_jobs) abort();
  memset(s_jobs, 0, sizeof(SceneJob) * kJobs);
  data_cache_hit_writeback_invalidate(s_jobs, sizeof(SceneJob) * kJobs);
}

void n64_scene_rsp_bg(const uint16_t *vram, const uint8_t *cache, uint16_t hs, uint16_t vs,
                      uint8_t xsc, int bpp, uint16_t tileadr, int r0, int r1, int prio,
                      int sx0, int sx1, int ox, int oy) {
  SceneJob *j = UncachedAddr(&s_jobs[s_next++ % kJobs]);
  j->vram = PhysicalAddr(vram);
  j->cache = PhysicalAddr(cache);
  j->hs = hs;
  j->vs = vs;
  j->xsc = xsc;
  j->wsh = bpp == 4 ? 4 : 3;
  j->tileadr = tileadr;
  j->r0 = (uint8_t)r0;
  j->r1 = (uint8_t)r1;
  j->prio = (uint8_t)prio;
  j->pad = 0;
  j->sx0 = (uint16_t)sx0;
  j->sx1 = (uint16_t)sx1;
  j->ox = (uint16_t)ox;
  j->oy = (uint16_t)oy;
  j->result = 0;
  rspq_write(s_ovl, 0, PhysicalAddr(j));
}
