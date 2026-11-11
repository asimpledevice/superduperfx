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

/* Audio: open the audio interface near the requested rate and return the
 * actual rate; pump fills every free output buffer with render(). */
int n64_platform_audio_init(int rate);
void n64_platform_audio_pump(void (*render)(int16_t *stereo, int frames));

/* ROM paging. Returns a read-only window onto the ROM file at `path` (after
 * any 512-byte copier header; *size_out is the remaining size), whose 8 KB
 * pages are copied in from the cartridge on first touch into `slots` RAM
 * slots, the oldest reused first. Reads through the window are the file's
 * bytes; writes fault. NULL when paging is unavailable. On the N64 the
 * window is TLB-mapped (kseg2) and a TLB miss pages in; the profile host
 * emulates it with mmap and SIGSEGV. */
enum { N64_ROM_PAGE_BYTES = 8192, N64_ROM_PAGE_MAX_SLOTS = 32 };
uint8_t *n64_platform_rom_window(const char *path, int *size_out, int slots);
/* Pages copied in so far. */
uint32_t n64_platform_rom_window_misses(void);

/* Code-cache pack (rom:/cc.pack, tools/n64/pack_code_cache.py): routine i of
 * g_recomp_routines is module i. open returns the module count (or -1) and
 * reports each module's memory size through size_cb; load copies module i
 * into newly allocated memory, relocates it and returns its entry (the base
 * to free in *base_out); free releases it. */
int n64_platform_codepack_open(const char *path, void (*size_cb)(uint32_t id, uint32_t bytes));
void *n64_platform_codepack_load(uint32_t id, void **base_out);
/* The same in two steps, one load at a time: begin allocates module id
 * (*base_out, NULL when out of memory) and starts its DMA; finish completes
 * it and returns its entry, or NULL if !wait and the DMA is still running
 * (or nothing is in flight). */
void *n64_platform_codepack_begin(uint32_t id, void **base_out);
void *n64_platform_codepack_finish(int wait);
void n64_platform_codepack_free(void *base);

/* Status text position: inside the area TVs show (overscan). */
enum { N64_STATUS_Y = 240 - 28 };

/* Sampling profiler (see n64_platform.c). Each sample also records
 * g_n64_profile_phase, the frame stage the host was in (always written, so
 * profiling and shipped builds have the same layout). */
enum { N64_PHASE_CPU = 0, N64_PHASE_PPU = 1, N64_PHASE_PRESENT = 2 };
extern volatile uint8_t g_n64_profile_phase;
/* Sets g_n64_profile_phase and, on the N64, writes its line back to RDRAM, so
 * an emulator's profiler reading RDRAM (ares icprof) sees the current stage
 * rather than whatever the write-back D-cache last flushed. */
void n64_platform_set_phase(uint8_t phase);
void n64_platform_profile_start(unsigned interval_us);
void n64_platform_profile_stop_and_dump(void);

#endif
