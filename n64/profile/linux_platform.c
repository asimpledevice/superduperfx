/* Desktop stand-in for n64_platform.c, used to profile the N64 host's exact
 * runtime configuration on x86 Linux (gprof, callgrind). No display: it runs
 * a fixed number of frames, reads the ROM from $N64_PROFILE_ROM, and exits.
 *
 * Input: none, or a replay. $N64_PROFILE_INPUT names a file of little-endian
 * 16-bit controller words, one per frame in the host's bit order (bit 0 B,
 * 1 Y, 2 Select, 3 Start, 4-7 Up/Down/Left/Right, 8 A, 9 X, 10 L, 11 R;
 * romtrace/bk2_pad.py converts a BizHawk movie), starting at host frame
 * $N64_PROFILE_INPUT_OFFSET (default 0; negative skips movie frames).
 * $N64_PROFILE_RAMLOG=N prints a few WRAM bytes every N frames ("RAM <game
 * frame> <replay index> <bytes>"):
 * the SMW progress bytes by default, or the hex addresses listed in
 * $N64_PROFILE_RAMLOG_ADDRS. */
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
static uint16_t *s_pad;
static long s_pad_len, s_pad_pos;
static unsigned s_ramlog;
static unsigned s_ramlog_addr[16], s_ramlog_n;
extern unsigned char g_ram[];
extern int snes_frame_counter;

static void replay_init(void) {
  static int done;
  if (done) return;
  done = 1;
  const char *path = getenv("N64_PROFILE_INPUT");
  if (path && path[0]) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    s_pad_len = ftell(f) / 2;
    fseek(f, 0, SEEK_SET);
    s_pad = malloc((size_t)s_pad_len * 2 + 2);
    s_pad_len = (long)fread(s_pad, 2, (size_t)s_pad_len, f);
    fclose(f);
    const char *o = getenv("N64_PROFILE_INPUT_OFFSET");
    s_pad_pos = o ? -atol(o) : 0;
    fprintf(stderr, "[replay] %ld frames from %s, offset %ld\n", s_pad_len, path, -s_pad_pos);
  }
  const char *r = getenv("N64_PROFILE_RAMLOG");
  s_ramlog = r ? (unsigned)atoi(r) : 0;
  const char *a = getenv("N64_PROFILE_RAMLOG_ADDRS");
  /* SMW: game mode, translevel, exits, lives, powerup, X position. */
  const char *def = "0100,13bf,1f2e,0dbe,0019,0094,0095";
  for (const char *p = (a && a[0]) ? a : def; *p && s_ramlog_n < 16;) {
    char *end;
    s_ramlog_addr[s_ramlog_n++] = (unsigned)strtoul(p, &end, 16) & 0x1ffff;
    p = *end ? end + 1 : end;
  }
}

uint32_t n64_platform_read_pad(void) {
  replay_init();
  if (s_ramlog && snes_frame_counter % s_ramlog == 0) {
    printf("RAM %d %ld", snes_frame_counter, s_pad_pos);
    for (unsigned i = 0; i < s_ramlog_n; i++) printf(" %02x", g_ram[s_ramlog_addr[i]]);
    printf("\n");
  }
  if (!s_pad) return 0;
  const long i = s_pad_pos++;
  return (i >= 0 && i < s_pad_len) ? s_pad[i] : 0;
}
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
volatile uint8_t g_n64_profile_phase;
void n64_platform_profile_start(unsigned interval_us) { (void)interval_us; }
void n64_platform_profile_stop_and_dump(void) {}

/* Audio: pull one frame's worth of samples (32000 Hz / 60) per call and log
 * a running hash every 30 frames, so APU changes can be checked like video. */
int n64_platform_audio_init(int rate) { return rate; }
/* Per-frame hook for profile/romtrace (weak: absent unless linked). */
extern void rt_frame(void) __attribute__((weak));
void n64_platform_audio_pump(void (*render)(int16_t *stereo, int frames)) {
  if (rt_frame) rt_frame();
  static uint32_t h = 2166136261u;
  static unsigned calls;
  int16_t buf[534 * 2];
  const int n = (calls % 3 == 2) ? 534 : 533;
  render(buf, n);
  for (int i = 0; i < n * 2; ++i) h = (h ^ (uint16_t)buf[i]) * 16777619u;
  if (calls % 30 == 0) printf("AUDIO %u %08x\n", calls, h);
  calls++;
}
