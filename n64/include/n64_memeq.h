#ifndef N64_MEMEQ_H
#define N64_MEMEQ_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* Whether n bytes at a and b are the same. newlib's memcmp goes byte by
 * byte on MIPS; the PPU's per-line questions ("the same registers as the
 * line above?") compare a few hundred bytes 224 times a frame. Words when
 * both are word aligned, memcmp otherwise and for the tail. */
static inline bool n64_mem_eq(const void *a, const void *b, size_t n) {
  if ((((uintptr_t)a | (uintptr_t)b) & 3) == 0) {
    typedef uint32_t __attribute__((may_alias)) A32;
    const A32 *x = (const A32 *)a, *y = (const A32 *)b;
    size_t w = n >> 2;
    for (; w >= 4; w -= 4, x += 4, y += 4)
      if ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) return false;
    for (; w; --w, ++x, ++y)
      if (*x != *y) return false;
    n &= 3;
    a = x; b = y;
  }
  return n == 0 || memcmp(a, b, n) == 0;
}
#endif
