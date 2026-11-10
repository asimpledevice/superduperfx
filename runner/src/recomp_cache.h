/* Code cache for compiled routines (generated with SNESRECOMP_EMIT_LEAN and
 * SNESRECOMP_EMIT_CODE_CACHE).
 *
 * Every compiled routine F is emitted as F__body, and dispatch_v2.c defines
 * F as a resident trampoline through a RecompRoutine record:
 *
 *   RecompReturn F(CpuState *cpu) { return recomp_call(&F__rt, cpu); }
 *
 * so callers, the dispatch table and host wrappers are unchanged. fn is the
 * entry while the routine is resident and NULL otherwise; a call to an absent
 * routine loads it first (recomp_cache_load), evicting the least recently
 * used routines that are not running until it fits the budget. A routine is
 * running while `active` is nonzero (calls in progress, on any stack), and a
 * running routine is never evicted.
 *
 * Where a body comes from is the host's business (recomp_cache_set_loader):
 * by default the body is linked into the program and "loading" only makes
 * it reachable, which is how the profile host emulates the cache exactly;
 * the N64 host loads bodies from the cartridge. Behaviour is the same either
 * way: a routine always runs once called, only later if it had to be loaded.
 */
#ifndef RECOMP_CACHE_H
#define RECOMP_CACHE_H

#include "cpu_state.h"

typedef RecompReturn (*RecompFn)(CpuState *cpu);

typedef struct RecompRoutine {
  RecompFn fn;          /* entry while resident, else NULL */
  RecompFn body;        /* the linked body (default loader) */
  const char *name;
  uint32_t size;        /* bytes it takes when resident (0: ask the host) */
  uint32_t last;        /* use stamp, for least-recently-used eviction */
  uint32_t active;      /* calls in progress */
  int32_t resident_idx; /* position in the resident list, -1 if absent */
  void *host;           /* the loader's handle while resident */
} RecompRoutine;

/* RECOMP_CACHE_EXTERNAL_BODIES: bodies are not linked into the program (the
 * N64 code cache loads them as modules), so records do not refer to them. */
#ifdef RECOMP_CACHE_EXTERNAL_BODIES
#define RECOMP_ROUTINE(body, name) { NULL, NULL, (name), 0, 0, 0, -1, NULL }
#else
#define RECOMP_ROUTINE(body, name) { NULL, (body), (name), 0, 0, 0, -1, NULL }
#endif

extern RecompRoutine *const g_recomp_routines[];
extern const unsigned g_recomp_routine_count;
extern uint32_t g_recomp_clock;

void recomp_cache_load(RecompRoutine *rt);

static inline RecompReturn recomp_call(RecompRoutine *rt, CpuState *cpu) {
  if (__builtin_expect(rt->fn == NULL, 0)) recomp_cache_load(rt);
  rt->last = ++g_recomp_clock;
  rt->active++;
  RecompReturn r = rt->fn(cpu);
  rt->active--;
  return r;
}

/* Budget in bytes for resident routines (0, the default: unlimited). */
void recomp_cache_set_capacity(uint32_t bytes);
/* Size of a routine when resident, by name (0: unknown). Default: 0, which
 * the cache counts as 1 KB. */
void recomp_cache_set_size_hook(uint32_t (*size_of)(const char *name));
/* Makes rt->fn valid (bytes come from wherever the host keeps bodies), or
 * releases what it made valid. Default: fn = body, nothing to release. */
void recomp_cache_set_loader(void (*load)(RecompRoutine *rt),
                             void (*unload)(RecompRoutine *rt));

typedef struct RecompCacheStats {
  uint32_t loads, evictions, resident, over_budget;
  uint64_t load_bytes, used_bytes;
} RecompCacheStats;
void recomp_cache_stats(RecompCacheStats *out);

#endif
