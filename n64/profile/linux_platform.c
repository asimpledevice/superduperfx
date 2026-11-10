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
#include <signal.h>
#include <string.h>
#include <sys/mman.h>

#include "n64_platform.h"
#include "recomp_cache.h"

static unsigned s_frames, s_limit;

/* Code cache emulation (code generated with SNESRECOMP_EMIT_CODE_CACHE):
 * $N64_PROFILE_CODE_CACHE is the budget in bytes, $N64_PROFILE_CODE_SIZES a
 * file of "<name> <bytes>" lines (MIPS sizes, e.g. from mips64-elf-nm -S on
 * the N64 objects). Statistics print every 600 frames. */
static char **s_cc_names; static uint32_t *s_cc_sizes; static unsigned s_cc_n;
static uint32_t cc_size_of(const char *name) {
  unsigned lo = 0, hi = s_cc_n;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    int c = strcmp(s_cc_names[mid], name);
    if (!c) return s_cc_sizes[mid];
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return 0;
}
static int cc_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
static unsigned s_cc_report;

/* Background loading (-DN64_PROFILE_CC_ASYNC='"cc_fallback_v2.c"', the table
 * from tools/n64/split_code_cache.py --fallback-only): a load is in flight
 * for $N64_PROFILE_CODE_ASYNC frames (default 1), and calls meanwhile run in
 * the interpreter fallback, as on the N64. */
#ifdef N64_PROFILE_CC_ASYNC
#include N64_PROFILE_CC_ASYNC
#include "../../runner/src/recomp_cache_interp.c"
static unsigned s_cc_delay = 1, s_cc_begin_frame;
static int cc_async_begin(RecompRoutine *rt) {
  if (g_recomp_fallback[rt->id].pc24 == 0xFFFFFFFFu) return 0;
  {  /* $N64_PROFILE_CODE_ASYNC_IDS=lo:hi: only these ids (bisection) */
    const char *r = getenv("N64_PROFILE_CODE_ASYNC_IDS");
    unsigned lo, hi;
    if (r && sscanf(r, "%u:%u", &lo, &hi) == 2 && (rt->id < lo || rt->id >= hi)) return 0;
  }
  s_cc_begin_frame = s_frames;
  if (getenv("N64_PROFILE_CODE_ASYNC_LOG"))
    fprintf(stderr, "[cc] frame %u: %s interpreted while loading\n", s_frames, rt->name);
  return 1;
}
static int cc_async_finish(RecompRoutine *rt, int wait) {
  if (!wait && s_frames - s_cc_begin_frame < s_cc_delay) return 0;
  rt->fn = rt->body;
  return 1;
}
static void cc_async_init(void) {
  const char *d = getenv("N64_PROFILE_CODE_ASYNC");
  if (d && d[0]) s_cc_delay = (unsigned)atoi(d);
  for (unsigned i = 0; i < g_recomp_routine_count; i++) g_recomp_routines[i]->id = i;
  recomp_cache_set_async(cc_async_begin, cc_async_finish, recomp_cache_interp_fallback);
}
#endif

void n64_platform_init(void) {
  const char *n = getenv("N64_PROFILE_FRAMES");
  s_limit = n ? (unsigned)atoi(n) : 300;
  const char *cap = getenv("N64_PROFILE_CODE_CACHE");
  if (cap && cap[0]) {
    recomp_cache_set_capacity((uint32_t)strtoul(cap, NULL, 0));
    s_cc_report = 1;
  }
  const char *sizes = getenv("N64_PROFILE_CODE_SIZES");
  FILE *f = sizes ? fopen(sizes, "r") : NULL;
  if (f) {
    char name[256]; unsigned long sz; unsigned capn = 0;
    char **names = NULL; uint32_t *szs = NULL;
    while (fscanf(f, "%255s %lu", name, &sz) == 2) {
      if (s_cc_n == capn) {
        capn = capn ? capn * 2 : 1024;
        names = realloc(names, sizeof *names * capn);
        szs = realloc(szs, sizeof *szs * capn);
      }
      names[s_cc_n] = strdup(name); szs[s_cc_n] = (uint32_t)sz; s_cc_n++;
    }
    fclose(f);
    /* Sort names, carrying sizes along. */
    char **pairs = malloc(sizeof *pairs * s_cc_n);
    for (unsigned i = 0; i < s_cc_n; i++) {
      pairs[i] = malloc(strlen(names[i]) + 16);
      sprintf(pairs[i], "%s %u", names[i], szs[i]);
    }
    qsort(pairs, s_cc_n, sizeof *pairs, cc_cmp);
    s_cc_names = names; s_cc_sizes = szs;
    for (unsigned i = 0; i < s_cc_n; i++) {
      char *sp = strrchr(pairs[i], ' ');
      *sp = 0;
      s_cc_names[i] = pairs[i];
      s_cc_sizes[i] = (uint32_t)strtoul(sp + 1, NULL, 10);
    }
    recomp_cache_set_size_hook(cc_size_of);
  }
#ifdef N64_PROFILE_CC_ASYNC
  cc_async_init();
#endif
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
  if (s_cc_report && snes_frame_counter % 600 == 0) {
    RecompCacheStats st;
    recomp_cache_stats(&st);
    printf("CODECACHE %d loads %u bytes %llu evictions %u resident %u used %llu over %u\n",
           snes_frame_counter, st.loads, (unsigned long long)st.load_bytes, st.evictions,
           st.resident, (unsigned long long)st.used_bytes, st.over_budget);
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

/* ROM paging, emulated: the window is an inaccessible mapping; a fault on
 * a page copies it in from the file image and makes it readable, and the
 * slot it takes is the oldest one's, which becomes inaccessible again. Same
 * page size, slot count and replacement order as the N64 TLB window, so the
 * miss counts match and the emulation is checked against plain loading. */
static uint8_t *s_win, *s_win_src;
static uint32_t s_win_bytes, s_win_misses;
static int s_win_slots, s_win_next;
static long *s_win_slot_page;

static void win_fault(int sig, siginfo_t *si, void *uc) {
  (void)uc;
  uint8_t *a = (uint8_t *)si->si_addr;
  if (!s_win || a < s_win || a >= s_win + s_win_bytes) {
    signal(sig, SIG_DFL);
    raise(sig);
    return;
  }
  const long page = (long)((a - s_win) / N64_ROM_PAGE_BYTES);
  const int k = s_win_next;
  s_win_next = (s_win_next + 1) % s_win_slots;
  if (s_win_slot_page[k] >= 0)
    mprotect(s_win + s_win_slot_page[k] * N64_ROM_PAGE_BYTES, N64_ROM_PAGE_BYTES, PROT_NONE);
  uint8_t *p = s_win + page * N64_ROM_PAGE_BYTES;
  mprotect(p, N64_ROM_PAGE_BYTES, PROT_READ | PROT_WRITE);
  uint32_t len = s_win_bytes - (uint32_t)(page * N64_ROM_PAGE_BYTES);
  if (len > N64_ROM_PAGE_BYTES) len = N64_ROM_PAGE_BYTES;
  memcpy(p, s_win_src + page * N64_ROM_PAGE_BYTES, len);
  mprotect(p, N64_ROM_PAGE_BYTES, PROT_READ);
  s_win_slot_page[k] = page;
  s_win_misses++;
}

uint8_t *n64_platform_rom_window(const char *path, int *size_out, int slots) {
  if (slots < 2 || slots > N64_ROM_PAGE_MAX_SLOTS || !path) return NULL;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  long skip = (size & 0x3ff) == 0x200 ? 0x200 : 0;   /* copier header */
  size -= skip;
  s_win_src = malloc((size_t)size);
  fseek(f, skip, SEEK_SET);
  if (fread(s_win_src, 1, (size_t)size, f) != (size_t)size) { fclose(f); return NULL; }
  fclose(f);
  const size_t span = ((size_t)size + N64_ROM_PAGE_BYTES - 1) / N64_ROM_PAGE_BYTES * N64_ROM_PAGE_BYTES;
  void *w = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (w == MAP_FAILED) return NULL;
  s_win = w;
  s_win_bytes = (uint32_t)size;
  s_win_slots = slots;
  s_win_slot_page = malloc(sizeof(long) * (size_t)slots);
  for (int i = 0; i < slots; i++) s_win_slot_page[i] = -1;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = win_fault;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &sa, NULL);
  *size_out = (int)size;
  return s_win;
}

uint32_t n64_platform_rom_window_misses(void) { return s_win_misses; }

/* No code pack here: the profile host links every body in. */
int n64_platform_codepack_open(const char *path, void (*size_cb)(uint32_t id, uint32_t bytes)) {
  (void)path; (void)size_cb; return -1;
}
void *n64_platform_codepack_load(uint32_t id, void **base_out) { (void)id; *base_out = NULL; return NULL; }
void n64_platform_codepack_free(void *base) { (void)base; }
void *n64_platform_codepack_begin(uint32_t id, void **base_out) { (void)id; *base_out = NULL; return NULL; }
void *n64_platform_codepack_finish(int wait) { (void)wait; return NULL; }
