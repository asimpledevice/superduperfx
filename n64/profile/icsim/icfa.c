/* icfa TRACE: misses of a fully associative LRU cache of 512 32-byte lines
 * (16 KB) on the same trace, per stage: the floor no placement in a
 * direct-mapped 16 KB cache can be expected to beat. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define N 512
static uint32_t line_of[N]; static uint64_t last[N]; static int used;
static int32_t slot_of[1u << 18];
int main(int argc, char **argv) {
  FILE *t = fopen(argv[1], "rb"); if (!t) return 1;
  for (uint32_t i = 0; i < (1u << 18); i++) slot_of[i] = -1;
  uint64_t miss[4] = {0}, now = 0; uint32_t buf[1 << 16]; size_t n;
  while ((n = fread(buf, 4, 1 << 16, t)) > 0)
    for (size_t i = 0; i < n; i++) {
      const uint32_t l = (buf[i] >> 5) & 0x3ffff, s = buf[i] & 3; now++;
      int sl = slot_of[l];
      if (sl >= 0) { last[sl] = now; continue; }
      miss[s]++;
      if (used < N) sl = used++;
      else { sl = 0; for (int k = 1; k < N; k++) if (last[k] < last[sl]) sl = k; slot_of[line_of[sl]] = -1; }
      line_of[sl] = l; last[sl] = now; slot_of[l] = sl;
    }
  for (int s = 0; s < 3; s++) printf("STAGE %d %llu\n", s, (unsigned long long)miss[s]);
  return 0;
}
