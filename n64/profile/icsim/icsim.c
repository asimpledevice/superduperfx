/* icsim TRACE [REMAP]: replay an ares icprof trace through the VR4300
 * I-cache (16 KB, direct-mapped, 32-byte lines) and count misses per stage.
 * REMAP: lines "old_start old_end new_start" (hex, physical): code in
 * [old_start, old_end) is simulated at new_start + (addr - old_start),
 * i.e. a different link layout. Prints "STAGE s misses" and, with
 * $ICSIM_LINES, writes per-original-line misses to that file. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { uint32_t lo, hi, to; } Range;
static Range *rg; static int nrg;
static int cmp(const void *a, const void *b) { const Range *x = a, *y = b; return x->lo < y->lo ? -1 : x->lo > y->lo; }
static uint32_t remap(uint32_t a) {
  int lo = 0, hi = nrg;
  while (lo < hi) { int m = (lo + hi) / 2; if (rg[m].lo <= a) lo = m + 1; else hi = m; }
  if (lo > 0 && a < rg[lo - 1].hi) return rg[lo - 1].to + (a - rg[lo - 1].lo);
  return a;
}
int main(int argc, char **argv) {
  FILE *t = fopen(argv[1], "rb"); if (!t) return 1;
  if (argc > 2) {
    FILE *r = fopen(argv[2], "r"); unsigned a, b, c; int cap = 0;
    while (fscanf(r, "%x %x %x", &a, &b, &c) == 3) {
      if (nrg == cap) { cap = cap ? cap * 2 : 1024; rg = realloc(rg, sizeof *rg * cap); }
      rg[nrg++] = (Range){a, b, c};
    }
    fclose(r); qsort(rg, nrg, sizeof *rg, cmp);
  }
  static uint32_t tag[512]; static uint8_t valid[512];
  static uint32_t lmiss[1u << 18];
  uint64_t miss[4] = {0}, acc = 0; uint32_t buf[1 << 16]; size_t n;
  while ((n = fread(buf, 4, 1 << 16, t)) > 0) {
    for (size_t i = 0; i < n; i++) {
      const uint32_t w = buf[i], orig = w & ~31u, a = remap(orig), s = w & 3;
      const uint32_t set = (a >> 5) & 511, tg = a >> 14;
      acc++;
      if (!valid[set] || tag[set] != tg) { valid[set] = 1; tag[set] = tg; miss[s]++; lmiss[(orig >> 5) & 0x3ffff]++; }
    }
  }
  printf("ACCESS %llu\n", (unsigned long long)acc);
  for (int s = 0; s < 4; s++) printf("STAGE %d %llu\n", s, (unsigned long long)miss[s]);
  const char *lf = getenv("ICSIM_LINES");
  if (lf) { FILE *o = fopen(lf, "w"); for (uint32_t i = 0; i < (1u << 18); i++) if (lmiss[i]) fprintf(o, "%08x %u\n", i << 5, lmiss[i]); fclose(o); }
  return 0;
}
