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

void recomp_enter_rt(const struct RecompRoutine *rt) { recomp_enter(rt->name); }

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

static int (*s_async_begin)(RecompRoutine *rt);
static int (*s_async_finish)(RecompRoutine *rt, int wait);
static RecompReturn (*s_fallback)(RecompRoutine *rt, CpuState *cpu);
static RecompRoutine *s_pending;    /* the load in flight */

void recomp_cache_set_async(int (*begin)(RecompRoutine *rt),
                            int (*finish)(RecompRoutine *rt, int wait),
                            RecompReturn (*fallback)(RecompRoutine *rt, CpuState *cpu)) {
  s_async_begin = begin;
  s_async_finish = finish;
  s_fallback = fallback;
}

/* Budget and record a routine as resident (its memory is about to be used). */
static void reserve(RecompRoutine *rt) {
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
  rt->resident_idx = (int32_t)s_nresident;
  s_resident[s_nresident++] = rt;
  s_used += size;
  s_stats.loads++;
  s_stats.load_bytes += size;
}

static void complete_pending(int wait) {
  if (s_pending && s_async_finish(s_pending, wait)) {
    s_pending->active--;   /* unpinned: loaded */
    s_pending = NULL;
  }
}

void recomp_cache_poll(void) { complete_pending(0); }

RecompReturn recomp_cache_call_absent(RecompRoutine *rt, CpuState *cpu) {
  if (!s_async_begin) {
    recomp_cache_load(rt);
    return recomp_call(rt, cpu);
  }
  complete_pending(0);
  if (rt->fn) return recomp_call(rt, cpu);
  if (rt == s_pending) {           /* still on its way: interpret again */
    s_stats.fallbacks++;
    return s_fallback(rt, cpu);
  }
  complete_pending(1);             /* one load in flight at a time */
  rt->active++;                    /* pinned while it loads */
  reserve(rt);
  s_pending = rt;
  if (!s_async_begin(rt)) {        /* cannot run before it is loaded */
    complete_pending(1);
    return recomp_call(rt, cpu);
  }
  s_stats.fallbacks++;
  return s_fallback(rt, cpu);
}

void recomp_cache_load(RecompRoutine *rt) {
  reserve(rt);
  s_load(rt);
}

void recomp_cache_stats(RecompCacheStats *out) {
  *out = s_stats;
  out->resident = s_nresident;
  out->used_bytes = s_used;
}
