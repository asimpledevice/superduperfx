/* libdragon side of the N64 host. See n64_platform.h. */
#include <libdragon.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "n64_platform.h"
#ifdef N64_RSPDSP
#include "snesdsp_rsp.h"
#endif

/* ---- Sampling profiler ------------------------------------------------
 * A continuous timer interrupt records COP0 EPC, the PC the CPU was
 * executing when the interrupt fired, with the frame stage in its two low
 * bits (instructions are 4-aligned). Samples are dumped over the debug log
 * as "PROF <pc> <stage>" lines; map them to functions offline with the ELF
 * (n64/profile/symbolize.py, icache_order.py). */
#ifndef N64_PROFILE_MAX
#define N64_PROFILE_MAX 131072
#endif
enum { kProfMax = N64_PROFILE_MAX };
static uint32_t s_prof_pc[kProfMax];
static volatile unsigned s_prof_n;
static timer_link_t *s_prof_timer;
volatile uint8_t g_n64_profile_phase;

static void prof_tick(int ovfl) {
  (void)ovfl;
  if (s_prof_n < kProfMax)
    s_prof_pc[s_prof_n++] = ((uint32_t)C0_EPC() & ~3u) | (g_n64_profile_phase & 3u);
}

void n64_platform_profile_start(unsigned interval_us) {
  s_prof_n = 0;
  if (!s_prof_timer)
    s_prof_timer = new_timer(TICKS_FROM_US(interval_us), TF_CONTINUOUS, prof_tick);
}

void n64_platform_profile_stop_and_dump(void) {
  if (s_prof_timer) { delete_timer(s_prof_timer); s_prof_timer = NULL; }
  unsigned n = s_prof_n;
  debugf("PROF_BEGIN %u\n", n);
  for (unsigned i = 0; i < n; ++i)
    debugf("PROF %08lx %lu\n", (unsigned long)(s_prof_pc[i] & ~3u), (unsigned long)(s_prof_pc[i] & 3u));
  debugf("PROF_END\n");
}

void n64_platform_init(void) {
  debug_init_isviewer();
  debug_init_usblog();
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  joypad_init();
  rdpq_init();
#ifdef N64_RSPDSP
  snesdsp_rsp_init();
#endif
#ifdef N64PPU_RDP_VALIDATE
  rdpq_debug_start();
#endif
  timer_init();
  dfs_init(DFS_DEFAULT_LOCATION);
  debugf("[n64] snesrecomp interpreter-only host, expansion pak: %s\n",
         is_memory_expanded() ? "yes" : "no");
}

int n64_platform_has_expansion_pak(void) { return is_memory_expanded() ? 1 : 0; }

void n64_platform_log(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  debugf("%s", buf);
}

void n64_platform_fatal(const char *message) {
  debugf("[n64] fatal: %s\n", message);
  console_init();
  console_set_render_mode(RENDER_MANUAL);
  console_clear();
  printf("snesrecomp fatal error:\n\n%s\n", message);
  console_render();
  for (;;) {}
}

uint64_t n64_platform_time_us(void) { return get_ticks_us(); }

int n64_platform_audio_init(int rate) {
  audio_init(rate, 4);
  return audio_get_frequency();
}

void n64_platform_audio_pump(void (*render)(int16_t *stereo, int frames)) {
  while (audio_can_write()) {
    short *buf = audio_write_begin();
    render(buf, audio_get_buffer_length());
    audio_write_end();
  }
}

uint32_t n64_platform_read_pad(void) {
  joypad_poll();
  joypad_buttons_t b = joypad_get_buttons(JOYPAD_PORT_1);
  joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
  const int dz = 32;
  uint32_t w = 0;
  if (b.a) w |= 1u << 0;         /* N64 A   -> SNES B */
  if (b.b) w |= 1u << 1;         /* N64 B   -> SNES Y */
  if (b.z) w |= 1u << 2;         /* Z       -> Select */
  if (b.start) w |= 1u << 3;
  if (b.d_up || in.stick_y > dz) w |= 1u << 4;
  if (b.d_down || in.stick_y < -dz) w |= 1u << 5;
  if (b.d_left || in.stick_x < -dz) w |= 1u << 6;
  if (b.d_right || in.stick_x > dz) w |= 1u << 7;
  if (b.c_right) w |= 1u << 8;   /* C-right -> SNES A */
  if (b.c_up) w |= 1u << 9;      /* C-up    -> SNES X */
  if (b.l) w |= 1u << 10;
  if (b.r) w |= 1u << 11;
  return w;
}

void n64_platform_present(const uint32_t *pixels, int width, int height,
                          const char *status) {
  surface_t *fb = display_get();
  const int ox = (fb->width - width) / 2, oy = (fb->height - height) / 2;
  for (int y = 0; y < height; ++y) {
    const uint32_t *src = pixels + (size_t)y * width;
    uint16_t *dst = (uint16_t *)((uint8_t *)fb->buffer + (size_t)(y + oy) * fb->stride) + ox;
    for (int x = 0; x < width; ++x) {
      uint32_t p = src[x];
      dst[x] = (uint16_t)((((p >> 19) & 31) << 11) | (((p >> 11) & 31) << 6) |
                          (((p >> 3) & 31) << 1) | 1);
    }
  }
  if (status && status[0]) {
    graphics_draw_box(fb, 16, N64_STATUS_Y - 2, fb->width - 32, 12, graphics_make_color(0, 0, 0, 255));
    graphics_set_color(graphics_make_color(255, 255, 0, 255), 0);
    graphics_draw_text(fb, 24, N64_STATUS_Y, status);
  }
  display_show(fb);
}

/* ---- ROM paging (n64_platform_rom_window) ---------------------------- */

/* The window lives in kseg2, which the VR4300 always translates through the
 * TLB in kernel mode. Each slot is one 8 KB-aligned RAM block mapped by one
 * TLB entry (an even/odd pair of 4 KB pages), entry i for slot i; libdragon
 * itself uses no TLB entries. Window pages are 8 KB-aligned too, so a window
 * address and its slot's kseg0 address share data-cache index bits 12:4 and
 * invalidating the slot through kseg0 clears whatever the previous page
 * left in the cache. */
#define ROM_WIN_VA 0xC0000000u
static uint8_t *s_rom_slots;            /* kseg0, N64_ROM_PAGE_BYTES-aligned */
static int s_rom_nslots, s_rom_next;
static pi_addr_t s_rom_pi;              /* PI address of the image's first byte */
static uint32_t s_rom_bytes, s_rom_misses;
static exception_handler_t s_rom_prev_handler;

static void rom_page_in(uint32_t page) {
  const int k = s_rom_next;
  s_rom_next = (s_rom_next + 1) % s_rom_nslots;
  uint8_t *slot = s_rom_slots + (size_t)k * N64_ROM_PAGE_BYTES;
  const uint32_t off = page * N64_ROM_PAGE_BYTES;
  uint32_t len = s_rom_bytes - off;
  if (len > N64_ROM_PAGE_BYTES) len = N64_ROM_PAGE_BYTES;
  /* Drop the previous page's lines (same physical tag and cache index). */
  data_cache_hit_invalidate(slot, N64_ROM_PAGE_BYTES);
  dma_read(slot, s_rom_pi + off, len);
  if (len < N64_ROM_PAGE_BYTES) {
    memset(slot + len, 0, N64_ROM_PAGE_BYTES - len);
    data_cache_hit_writeback(slot + len, N64_ROM_PAGE_BYTES - len);
  }
  const uint32_t pfn = PhysicalAddr(slot) >> 12;
  /* EntryLo: PFN, cacheable non-coherent (3), not writable (D = 0), valid,
   * global. */
  const uint32_t lo = (3u << 3) | (1u << 1) | 1u;
  C0_WRITE_INDEX(k);
  C0_WRITE_PAGEMASK(0);
  C0_WRITE_ENTRYHI(ROM_WIN_VA + off);
  C0_WRITE_ENTRYLO0((pfn << 6) | lo);
  C0_WRITE_ENTRYLO1(((pfn + 1) << 6) | lo);
  C0_TLBWI();
  s_rom_misses++;
}

static void rom_exception_handler(exception_t *ex) {
  if (ex->code == EXCEPTION_CODE_TLB_LOAD_I_MISS) {
    const uint32_t bva = (uint32_t)C0_BADVADDR();
    if (bva - ROM_WIN_VA < s_rom_bytes) {
      rom_page_in((bva - ROM_WIN_VA) / N64_ROM_PAGE_BYTES);
      return;   /* libdragon returns to the faulting instruction */
    }
  }
  if (s_rom_prev_handler) s_rom_prev_handler(ex);
}

uint8_t *n64_platform_rom_window(const char *path, int *size_out, int slots) {
  if (slots < 2 || slots > N64_ROM_PAGE_MAX_SLOTS) return NULL;
  const char *name = strncmp(path, "rom:/", 5) == 0 ? path + 5 : path;
  pi_addr_t pi = dfs_rom_addr(name);
  int size = pi ? dfs_rom_size(name) : -1;
  if (!pi || size <= 0) return NULL;
  if ((size & 0x3ff) == 0x200) { pi += 0x200; size -= 0x200; }  /* copier header */
  s_rom_slots = memalign(N64_ROM_PAGE_BYTES, (size_t)slots * N64_ROM_PAGE_BYTES);
  if (!s_rom_slots) return NULL;
  s_rom_nslots = slots;
  s_rom_next = 0;
  s_rom_pi = pi;
  s_rom_bytes = (uint32_t)size;
  /* Every entry invalid and distinct: kseg0 addresses never go through the
   * TLB, so these never match (two matching entries would shut it down). */
  for (int i = 0; i < 32; i++) {
    C0_WRITE_INDEX(i);
    C0_WRITE_PAGEMASK(0);
    C0_WRITE_ENTRYHI(0x80000000u + (uint32_t)i * N64_ROM_PAGE_BYTES);
    C0_WRITE_ENTRYLO0(0);
    C0_WRITE_ENTRYLO1(0);
    C0_TLBWI();
  }
  s_rom_prev_handler = register_exception_handler(rom_exception_handler);
  *size_out = size;
  return (uint8_t *)(uintptr_t)ROM_WIN_VA;
}

uint32_t n64_platform_rom_window_misses(void) { return s_rom_misses; }

/* ---- Loadable modules (code cache) ----------------------------------- */

void *n64_platform_module_open(const char *path) { return dlopen(path, RTLD_LOCAL); }
void *n64_platform_module_symbol(void *module, const char *name) { return dlsym(module, name); }
void n64_platform_module_close(void *module) { dlclose(module); }

