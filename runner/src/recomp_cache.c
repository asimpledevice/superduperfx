/* Code cache manager (see recomp_cache.h). */
#include "recomp_cache.h"

#include <stdlib.h>

uint32_t g_recomp_clock;

static uint32_t s_capacity;                 /* 0: unlimited */
static uint64_t s_used;
static RecompRoutine **s_resident;          /* resident routines, unordered */
static uint32_t s_nresident, s_resident_cap;
static uint32_t (*s_size_of)(const char *name);
static RecompCacheStats s_stats;

static void default_load(RecompRoutine *rt) { rt->fn = rt->body; }
static void default_unload(RecompRoutine *rt) { (void)rt; }
static void (*s_load)(RecompRoutine *rt) = default_load;
static void (*s_unload)(RecompRoutine *rt) = default_unload;

void recomp_cache_set_capacity(uint32_t bytes) { s_capacity = bytes; }
void recomp_cache_set_size_hook(uint32_t (*size_of)(const char *name)) { s_size_of = size_of; }
void recomp_cache_set_loader(void (*load)(RecompRoutine *rt),
                             void (*unload)(RecompRoutine *rt)) {
  s_load = load ? load : default_load;
  s_unload = unload ? unload : default_unload;
}

static uint32_t routine_size(RecompRoutine *rt) {
  if (!rt->size) {
    rt->size = s_size_of ? s_size_of(rt->name) : 0;
    if (!rt->size) rt->size = 1024;
  }
  return rt->size;
}

/* Evict the least recently used resident routine that is not running.
 * Returns 0 when every resident routine is running. */
static int evict_one(void) {
  int32_t best = -1;
  for (uint32_t i = 0; i < s_nresident; i++) {
    RecompRoutine *r = s_resident[i];
    if (r->active) continue;
    /* Stamps wrap; compare by age from the current clock. */
    if (best < 0 || (uint32_t)(g_recomp_clock - r->last) >
                        (uint32_t)(g_recomp_clock - s_resident[best]->last))
      best = (int32_t)i;
  }
  if (best < 0) return 0;
  RecompRoutine *victim = s_resident[best];
  s_unload(victim);
  victim->fn = NULL;
  victim->resident_idx = -1;
  s_used -= victim->size;
  s_resident[best] = s_resident[--s_nresident];
  s_resident[best]->resident_idx = best;
  s_stats.evictions++;
  return 1;
}

void recomp_cache_load(RecompRoutine *rt) {
  const uint32_t size = routine_size(rt);
  if (s_capacity) {
    while (s_used + size > s_capacity) {
      if (!evict_one()) { s_stats.over_budget++; break; }
    }
  }
  if (s_nresident == s_resident_cap) {
    s_resident_cap = s_resident_cap ? s_resident_cap * 2 : 256;
    s_resident = realloc(s_resident, sizeof *s_resident * s_resident_cap);
  }
  s_load(rt);
  rt->resident_idx = (int32_t)s_nresident;
  s_resident[s_nresident++] = rt;
  s_used += size;
  s_stats.loads++;
  s_stats.load_bytes += size;
}

void recomp_cache_stats(RecompCacheStats *out) {
  *out = s_stats;
  out->resident = s_nresident;
  out->used_bytes = s_used;
}
