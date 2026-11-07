/* Force-included into every runtime unit in the N64 build (-include).
 * Fills POSIX gaps in libdragon's newlib that the shared runtime relies on.
 * Implementations live in n64/src/n64_compat.c. */
#ifndef SNESRECOMP_N64_COMPAT_H
#define SNESRECOMP_N64_COMPAT_H

#include <time.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC ((clockid_t)4)
#endif

#ifdef __cplusplus
extern "C" {
#endif
int clock_gettime(clockid_t clock_id, struct timespec *tp);
#ifdef __cplusplus
}
#endif

#endif
