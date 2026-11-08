/* libdragon side of the N64 host. See n64_platform.h. */
#include <libdragon.h>
#include <stdarg.h>
#include <stdio.h>

#include "n64_platform.h"

/* ---- Sampling profiler ------------------------------------------------
 * A continuous timer interrupt records COP0 EPC, the PC the CPU was
 * executing when the interrupt fired. Samples are dumped over the debug log
 * as "PROF <pc>" lines; map them to functions offline with the ELF
 * (n64/profile/symbolize.py). */
enum { kProfMax = 32768 };
static uint32_t s_prof_pc[kProfMax];
static volatile unsigned s_prof_n;
static timer_link_t *s_prof_timer;

static void prof_tick(int ovfl) {
  (void)ovfl;
  if (s_prof_n < kProfMax) s_prof_pc[s_prof_n++] = (uint32_t)C0_EPC();
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
  for (unsigned i = 0; i < n; ++i) debugf("PROF %08lx\n", (unsigned long)s_prof_pc[i]);
  debugf("PROF_END\n");
}

void n64_platform_init(void) {
  debug_init_isviewer();
  debug_init_usblog();
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  joypad_init();
  rdpq_init();
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
    graphics_draw_box(fb, 0, fb->height - 12, fb->width, 12, graphics_make_color(0, 0, 0, 255));
    graphics_set_color(graphics_make_color(255, 255, 0, 255), 0);
    graphics_draw_text(fb, 8, fb->height - 10, status);
  }
  display_show(fb);
}
