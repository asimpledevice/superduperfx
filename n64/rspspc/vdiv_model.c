/* Exact 16/8 division as the RSP would do it: R = VRCP(d) (~2^31/d, ares's
 * table), R16 = (R >> 15) & 0xffff, q0 = (n * R16) >> 16 (VMUDL), r0 = n - q0*d
 * (VMUDN low 16 bits), then correct while r >= d. Reports q0's error and the
 * most corrections needed, over every n < 65536 and d in 1..255. */
#include <stdio.h>
#include <stdint.h>
static uint16_t tab[512];
static uint32_t vrcp16(int16_t in) {
  int32_t input = in, mask = input >> 31, data = input ^ mask; if (input > -32768) data -= mask;
  if (!data) return 0x7fffffff;
  unsigned shift = __builtin_clz((uint32_t)data);
  unsigned index = (unsigned)(((uint64_t)data << shift & 0x7fc00000u) >> 22);
  int64_t r = (0x10000 | tab[index]) << 14;
  return (uint32_t)(((int32_t)((uint32_t)r >> (31 - shift))) ^ mask);
}
int main(void) {
  tab[0] = 0xffff;
  for (int i = 1; i < 512; i++) { uint64_t a = i + 512, b = (1ull << 34) / a; tab[i] = (uint16_t)((b + 1) >> 8); }
  int emin = 0, emax = 0, cmax = 0;
  for (uint32_t d = 1; d < 256; d++) {
    const uint32_t R = vrcp16((int16_t)d), R16 = (R >> 15) & 0xffff;
    for (uint32_t n = 0; n < 65536; n++) {
      uint32_t q = (n * R16) >> 16;
      const int e = (int)q - (int)(n / d); if (e < emin) emin = e; if (e > emax) emax = e;
      uint32_t r = (n - ((q * d) & 0xffff)) & 0xffff; int c = 0;
      while (r >= d) { q++; r -= d; c++; }
      if (q != n / d || r != n % d) { printf("WRONG n %u d %u\n", n, d); return 1; }
      if (c > cmax) cmax = c;
    }
  }
  printf("q0 error %d..%d; at most %d corrections; all 16.7M results exact\n", emin, emax, cmax);
  return 0;
}
