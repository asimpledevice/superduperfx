/* Desktop stand-in for n64_platform.c, used to profile the N64 host's exact
 * runtime configuration on x86 Linux (gprof, callgrind). No display or pad:
 * it runs a fixed number of frames with no input, reads the ROM from
 * $N64_PROFILE_ROM, and exits. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "n64_platform.h"

static unsigned s_frames, s_limit;

void n64_platform_init(void) {
  const char *n = getenv("N64_PROFILE_FRAMES");
  s_limit = n ? (unsigned)atoi(n) : 300;
}
int n64_platform_has_expansion_pak(void) { return 1; }
void n64_platform_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}
void n64_platform_fatal(const char *message) {
  fprintf(stderr, "fatal: %s\n", message);
  exit(1);
}
uint64_t n64_platform_time_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
uint32_t n64_platform_read_pad(void) { return 0; }
void n64_platform_present(const uint32_t *pixels, int width, int height,
                          const char *status) {
  (void)status;
  if (s_frames % 30 == 0) {
    uint32_t h = 2166136261u;  /* FNV-1a over the visible frame */
    for (int i = 0; i < width * height; ++i) {
      h = (h ^ (pixels[i] & 0xffffff)) * 16777619u;
    }
    printf("HASH %u %08x\n", s_frames, h);
  }
  if (++s_frames >= s_limit) {
    const char *dump = getenv("N64_PROFILE_DUMP");
    if (dump) {
      FILE *f = fopen(dump, "wb");
      if (f) {
        fprintf(f, "P6\n%d %d\n255\n", width, height);
        for (int i = 0; i < width * height; ++i) {
          unsigned char rgb[3] = {(unsigned char)(pixels[i] >> 16),
                                  (unsigned char)(pixels[i] >> 8),
                                  (unsigned char)pixels[i]};
          fwrite(rgb, 1, 3, f);
        }
        fclose(f);
      }
    }
    exit(0);
  }
}

/* The sampling profiler is N64-only; use gprof or callgrind on desktop. */
void n64_platform_profile_start(unsigned interval_us) { (void)interval_us; }
void n64_platform_profile_stop_and_dump(void) {}

/* Audio: pull one frame's worth of samples (32000 Hz / 60) per call and log
 * a running hash every 30 frames, so APU changes can be checked like video. */
int n64_platform_audio_init(int rate) { return rate; }
void n64_platform_audio_pump(void (*render)(int16_t *stereo, int frames)) {
  static uint32_t h = 2166136261u;
  static unsigned calls;
  int16_t buf[534 * 2];
  const int n = (calls % 3 == 2) ? 534 : 533;
  render(buf, n);
  for (int i = 0; i < n * 2; ++i) h = (h ^ (uint16_t)buf[i]) * 16777619u;
  if (calls % 30 == 0) printf("AUDIO %u %08x\n", calls, h);
  calls++;
}
