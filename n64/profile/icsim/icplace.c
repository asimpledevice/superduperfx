/* icplace TRACE UNITS [STEP [TOP]]: what a loader could gain by choosing the
 * I-cache position of code placed at run time. UNITS: lines "lo hi name"
 * (hex physical addresses): each unit can be moved by a multiple of 32
 * bytes modulo 16 KB; everything else stays. Replays the icprof trace
 * (icsim's format) through the VR4300 I-cache, then, for the TOP units by
 * the misses on their lines and on the lines they evict (default 40), tries
 * every STEP-th line offset (default 4) with the others held, twice over.
 * Prints the misses before and after and the chosen offsets.
 * $ICPLACE_W=file writes the fixed code's accesses by line index;
 * $ICPLACE_OFFS=file ("lo offset" lines) evaluates given offsets instead,
 * for trying a placement rule (docs/n64/TODO.md, 2026-11-25). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t lo, hi; char name[64]; int off; uint64_t acc, miss, evict; } Unit;
static Unit *u; static int nu;
static uint32_t *ln; static uint16_t *un; static uint8_t *st; static size_t n;
static int cmp(const void *a, const void *b) { const Unit *x = a, *y = b; return x->lo < y->lo ? -1 : x->lo > y->lo; }
static uint64_t run(uint64_t *per_stage, int stats) {
  static uint32_t tag[512]; static int owner[512];
  memset(tag, 0xff, sizeof tag);
  for (int i = 0; i < 512; i++) owner[i] = -1;
  uint64_t miss = 0;
  if (per_stage) memset(per_stage, 0, 4 * sizeof *per_stage);
  for (size_t i = 0; i < n; i++) {
    const int k = un[i];
    const uint32_t a = ln[i] + (k != 0xffff ? (uint32_t)u[k].off : 0);
    const uint32_t set = a & 511;
    if (tag[set] != a) {
      tag[set] = a; miss++;
      if (per_stage) per_stage[st[i]]++;
      if (stats) {
        if (k != 0xffff) u[k].miss++;
        if (owner[set] >= 0 && owner[set] != k) u[owner[set]].evict++;   /* a unit's line thrown out by other code */
        if (k != 0xffff && owner[set] != k) u[k].evict++;                /* a unit throwing out other code */
      }
      owner[set] = k == 0xffff ? -1 : k;
    }
  }
  return miss;
}
int main(int argc, char **argv) {
  FILE *t = fopen(argv[1], "rb"); if (!t) return 1;
  fseek(t, 0, SEEK_END); n = (size_t)ftell(t) / 4; fseek(t, 0, SEEK_SET);
  uint32_t *w = malloc(n * 4); if (fread(w, 4, n, t) != n) return 1; fclose(t);
  FILE *f = fopen(argv[2], "r"); char buf[256]; int cap = 0;
  while (fgets(buf, sizeof buf, f)) {
    Unit x; memset(&x, 0, sizeof x);
    if (sscanf(buf, "%x %x %63s", &x.lo, &x.hi, x.name) < 2) continue;
    if (nu == cap) { cap = cap ? cap * 2 : 256; u = realloc(u, sizeof *u * cap); }
    u[nu++] = x;
  }
  qsort(u, nu, sizeof *u, cmp);
  const int step = argc > 3 ? atoi(argv[3]) : 4, top = argc > 4 ? atoi(argv[4]) : 40;
  ln = malloc(n * 4); un = malloc(n * 2); st = malloc(n);
  for (size_t i = 0; i < n; i++) {
    const uint32_t a = w[i] & ~31u;
    ln[i] = a >> 5; st[i] = w[i] & 3;
    int lo = 0, hi = nu;
    while (lo < hi) { int m = (lo + hi) / 2; if (u[m].lo <= a) lo = m + 1; else hi = m; }
    un[i] = (lo > 0 && a < u[lo - 1].hi) ? (uint16_t)(lo - 1) : 0xffff;
    if (un[i] != 0xffff) u[un[i]].acc++;
  }
  uint64_t ps[4];
  /* $ICPLACE_W: write, per cache line index, the accesses to code outside
   * the units (the fixed code's heat). $ICPLACE_OFFS: lines "lo offset"
   * (hex, decimal): evaluate those offsets and stop. */
  if (getenv("ICPLACE_W")) {
    static uint64_t wt[512];
    for (size_t i = 0; i < n; i++) if (un[i] == 0xffff && st[i] == 0) wt[ln[i] & 511]++;
    FILE *o = fopen(getenv("ICPLACE_W"), "w");
    for (int i = 0; i < 512; i++) fprintf(o, "%llu\n", (unsigned long long)wt[i]);
    fclose(o);
  }
  if (getenv("ICPLACE_OFFS")) {
    FILE *o = fopen(getenv("ICPLACE_OFFS"), "r"); unsigned lo; int off;
    while (fscanf(o, "%x %d", &lo, &off) == 2)
      for (int i = 0; i < nu; i++) if (u[i].lo == lo) u[i].off = off;
    fclose(o);
    const uint64_t m = run(ps, 0);
    printf("misses %llu (stages %llu %llu %llu %llu)\n", (unsigned long long)m, (unsigned long long)ps[0],
           (unsigned long long)ps[1], (unsigned long long)ps[2], (unsigned long long)ps[3]);
    return 0;
  }
  const uint64_t base = run(ps, 1);
  uint64_t inunits = 0; for (int i = 0; i < nu; i++) inunits += u[i].miss;
  printf("accesses %zu, misses %llu (stages %llu %llu %llu %llu); on units' lines %llu\n", n, (unsigned long long)base,
         (unsigned long long)ps[0], (unsigned long long)ps[1], (unsigned long long)ps[2], (unsigned long long)ps[3], (unsigned long long)inunits);
  int *ord = malloc(sizeof(int) * nu);
  for (int i = 0; i < nu; i++) ord[i] = i;
  for (int i = 0; i < nu; i++) for (int j = i + 1; j < nu; j++)
    if (u[ord[j]].evict > u[ord[i]].evict) { int t2 = ord[i]; ord[i] = ord[j]; ord[j] = t2; }
  uint64_t best = base;
  for (int pass = 0; pass < 2; pass++) {
    for (int r = 0; r < top && r < nu; r++) {
      Unit *x = &u[ord[r]];
      int bo = x->off;
      for (int o = 0; o < 512; o += step) {
        x->off = o;
        const uint64_t m = run(NULL, 0);
        if (m < best) { best = m; bo = o; }
      }
      x->off = bo;
    }
    printf("pass %d: misses %llu (%.1f%% of the base)\n", pass + 1, (unsigned long long)best, 100.0 * best / base);
    fflush(stdout);
  }
  run(ps, 0);
  printf("after: stages %llu %llu %llu %llu\n", (unsigned long long)ps[0], (unsigned long long)ps[1], (unsigned long long)ps[2], (unsigned long long)ps[3]);
  for (int r = 0; r < top && r < nu; r++)
    printf("  %-44s %6u bytes  misses %7llu  involved %7llu  -> +%d lines\n", u[ord[r]].name, u[ord[r]].hi - u[ord[r]].lo,
           (unsigned long long)u[ord[r]].miss, (unsigned long long)u[ord[r]].evict, u[ord[r]].off);
  return 0;
}
