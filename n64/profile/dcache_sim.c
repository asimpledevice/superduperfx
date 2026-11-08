/* Replay a VR4300 data-access trace through an 8 KB direct-mapped,
 * write-back, write-allocate data cache with 16-byte lines.
 *
 * The trace comes from ares's local data-cache profiler ($ARES_DCTRACE): one
 * little-endian u32 per load or store, the physical address in bits 0-22 and
 * bit 31 set for a store.
 *
 *   dcache_sim TRACE [MOVE...] [--sweep START:LEN]
 *
 * MOVE is START:LEN:NEWSTART (hex): accesses in [START, START+LEN) are
 * replayed at NEWSTART + offset, as if that object were placed there.
 * --sweep tries every 16-byte cache position for the range START:LEN and
 * prints the misses for each. Moved ranges are replayed above 16 MB, outside
 * RDRAM, so they can only collide with other data through the cache index,
 * never by sharing an address.
 *
 * Output: misses and dirty writebacks (each costs a 40-cycle line transfer
 * in ares's model).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t start, len, to; } Move;

static uint32_t *g_trace;
static size_t g_n;

static void simulate(const Move *moves, int nmoves, uint64_t *misses, uint64_t *writebacks) {
  static uint32_t tag[512];
  static uint8_t valid[512], dirty[512];
  memset(valid, 0, sizeof valid);
  memset(dirty, 0, sizeof dirty);
  uint64_t m = 0, wb = 0;
  for (size_t i = 0; i < g_n; i++) {
    uint32_t w = g_trace[i];
    uint32_t a = w & 0x7fffffu;
    for (int k = 0; k < nmoves; k++)
      if (a - moves[k].start < moves[k].len) { a = moves[k].to + (a - moves[k].start); break; }
    const uint32_t line = a >> 4, set = line & 511;
    if (!valid[set] || tag[set] != line) {
      m++;
      if (valid[set] && dirty[set]) wb++;
      tag[set] = line; valid[set] = 1; dirty[set] = 0;
    }
    if (w >> 31) dirty[set] = 1;
  }
  *misses = m;
  *writebacks = wb;
}

static Move parse_move(const char *s) {
  Move mv = {0, 0, 0};
  if (sscanf(s, "%x:%x:%x", &mv.start, &mv.len, &mv.to) < 2) {
    fprintf(stderr, "bad range %s\n", s);
    exit(1);
  }
  mv.start &= 0x7fffffu;
  mv.to &= 0x7fffffu;
  return mv;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: dcache_sim TRACE [START:LEN:NEWSTART...] [--sweep START:LEN]\n");
    return 1;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  fseek(f, 0, SEEK_END);
  g_n = (size_t)ftell(f) / 4;
  fseek(f, 0, SEEK_SET);
  g_trace = malloc(g_n * 4);
  if (!g_trace || fread(g_trace, 4, g_n, f) != g_n) { fprintf(stderr, "read failed\n"); return 1; }
  fclose(f);

  Move moves[64];
  int nmoves = 0, sweep = -1;
  for (int i = 2; i < argc && nmoves < 63; i++) {
    if (!strcmp(argv[i], "--sweep") && i + 1 < argc) {
      sweep = nmoves;
      moves[nmoves++] = parse_move(argv[++i]);
    } else {
      Move mv = parse_move(argv[i]);
      mv.to += 0x1000000u;  /* above RDRAM: index collisions only */
      moves[nmoves++] = mv;
    }
  }
  uint64_t m, wb;
  if (sweep < 0) {
    simulate(moves, nmoves, &m, &wb);
    printf("accesses %zu misses %llu writebacks %llu\n", g_n, (unsigned long long)m,
           (unsigned long long)wb);
    return 0;
  }
  /* Keep the moved range at the same address modulo 8 KB plus a 16-byte step. */
  const uint32_t base = moves[sweep].start;
  for (uint32_t step = 0; step < 8192; step += 16) {
    moves[sweep].to = 0x2000000u + (base & ~8191u) + ((base + step) & 8191u);
    simulate(moves, nmoves, &m, &wb);
    printf("%u %llu %llu\n", step, (unsigned long long)m, (unsigned long long)wb);
  }
  return 0;
}
