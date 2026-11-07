/* POSIX pieces libdragon's newlib lacks. Declared in n64/include/n64_compat.h. */
#include <errno.h>
#include <libdragon.h>
#include <time.h>

#include "n64_compat.h"

/* Every clock is the CPU counter here; the runtime only uses this for
 * elapsed-time measurements. */
int clock_gettime(clockid_t clock_id, struct timespec *tp) {
  (void)clock_id;
  if (!tp) {
    errno = EINVAL;
    return -1;
  }
  uint64_t us = get_ticks_us();
  tp->tv_sec = (time_t)(us / 1000000u);
  tp->tv_nsec = (long)((us % 1000000u) * 1000u);
  return 0;
}
