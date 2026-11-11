/* trg TRACE LINEMAP OUT: temporal relationship graph (Gloy & Smith) over
 * placement items. LINEMAP: u32 per 32-byte line of 8 MB (item id, or
 * 0xFFFFFFFF to ignore). For each access to item x (same stage as before),
 * every distinct item y used since x's previous use -- within the last K
 * distinct items -- gets w(x,y)++: had x and y shared cache sets, that
 * access to x would have missed. OUT: "x y w" lines (x < y, summed). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define K 160
#define HS (1u << 24)
static uint64_t *hk; static uint32_t *hv;
static void add(uint32_t a, uint32_t b) {
  if (a > b) { uint32_t t = a; a = b; b = t; }
  const uint64_t key = ((uint64_t)a << 32 | b) + 1;
  uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 40) & (HS - 1);
  while (hk[h] && hk[h] != key) h = (h + 1) & (HS - 1);
  hk[h] = key; hv[h]++;
}
int main(int argc, char **argv) {
  FILE *t = fopen(argv[1], "rb"), *m = fopen(argv[2], "rb");
  static uint32_t map[1u << 18];
  if (!t || !m || fread(map, 4, 1u << 18, m) != (1u << 18)) return 1;
  hk = calloc(HS, 8); hv = calloc(HS, 4);
  uint32_t q[K]; uint8_t qs[K]; int qn = 0;   /* recency queue, most recent first */
  uint32_t buf[1 << 16]; size_t n; uint32_t last = ~0u; uint64_t acc = 0;
  while ((n = fread(buf, 4, 1 << 16, t)) > 0)
    for (size_t i = 0; i < n; i++) {
      const uint32_t x = map[(buf[i] >> 5) & 0x3ffff]; const uint8_t s = buf[i] & 3;
      if (x == 0xFFFFFFFFu || x == last) continue;
      last = x; acc++;
      int k = 0;
      while (k < qn && q[k] != x) {
        if (qs[k] == s) add(x, q[k]);
        k++;
      }
      if (k == qn) { if (qn < K) qn++; k = qn - 1; }   /* not found: drop the oldest */
      memmove(q + 1, q, k * sizeof *q); memmove(qs + 1, qs, k);
      q[0] = x; qs[0] = s;
    }
  FILE *o = fopen(argv[3], "w");
  for (uint32_t h = 0; h < HS; h++) if (hk[h]) {
    const uint64_t k = hk[h] - 1;
    fprintf(o, "%u %u %u\n", (uint32_t)(k >> 32), (uint32_t)k, hv[h]);
  }
  fclose(o);
  fprintf(stderr, "trg: %llu item accesses\n", (unsigned long long)acc);
  return 0;
}
