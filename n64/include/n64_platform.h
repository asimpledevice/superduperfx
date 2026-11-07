/* The libdragon side of the N64 host.
 *
 * libdragon and the SNES runtime both define dma_read/dma_write, so no file
 * may include <libdragon.h> together with the runtime's snes/ headers. The
 * runtime-facing host (n64_host.c) talks to the console only through this
 * header; n64_platform.c is the only file that includes <libdragon.h>. */
#ifndef SNESRECOMP_N64_PLATFORM_H
#define SNESRECOMP_N64_PLATFORM_H

#include <stdint.h>

void n64_platform_init(void);
int n64_platform_has_expansion_pak(void);
void n64_platform_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void n64_platform_fatal(const char *message) __attribute__((noreturn));
uint64_t n64_platform_time_us(void);

/* Packed SNES pad word for RtlRunFrame: bit 0 B, 1 Y, 2 Select, 3 Start,
 * 4 Up, 5 Down, 6 Left, 7 Right, 8 A, 9 X, 10 L, 11 R. */
uint32_t n64_platform_read_pad(void);

/* Converts an XRGB8888 frame to the 16-bit display, centred, with an
 * optional status line along the bottom, and shows it. */
void n64_platform_present(const uint32_t *pixels, int width, int height,
                          const char *status);

#endif
