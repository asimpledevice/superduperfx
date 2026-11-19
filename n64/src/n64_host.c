/*
 * Interpreter-only N64 host for snesrecomp.
 *
 * Boots a SNES ROM with no recompiled code: every guest instruction runs
 * through the interpreter tier. This is the bring-up baseline for the N64
 * port. It measures how fast the shared runtime (interpreter, PPU, APU)
 * runs on a VR4300 before any native code is added.
 *
 * The ROM is read from the cartridge filesystem at rom:/game.sfc. The
 * Makefile packs it there from ROM=path/to/game.sfc; no ROM is committed.
 *
 * Video: the shared software PPU renders 256x224 XRGB8888, which is
 * converted to a 16-bit 320x240 framebuffer on the CPU. That conversion is
 * a stopgap until an RDP backend exists.
 * Audio: not output yet. The APU still runs, because guest code talks to it.
 */

#include "time_cats.h"
#include "n64_platform.h"
#include "blue_thread.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "beam_frame_driver.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/dma.h"
#include "snes/apu.h"
#include "n64_ppu_build.h"
#include "snes/cart.h"
#include "snes/spc.h"
#include "n64_ppu_rdp.h"

/* N64_RENDER: 0 = software PPU only, 1 = RDP backend (software PPU for
 * lines it cannot do), 2 = RDP backend plus the software PPU on every line,
 * comparing the two and logging differences (debug). */
#ifndef N64_RENDER
#define N64_RENDER 1
#endif
/* Draw each frame after the next frame's emulation, so the RSP expands its
 * backgrounds meanwhile (one frame more display latency; emulation is
 * unchanged). Default with the RSP background path. */
#ifndef N64_PIPELINE
#if N64_RENDER && defined(N64_RSPBG)
#define N64_PIPELINE 1
#else
#define N64_PIPELINE 0
#endif
#endif
static N64PpuFrame *s_rdp_frame;
static uint32_t *s_pixels;  /* defined below: the software PPU output */

/* The frame description (n64_ppu_desc.c): bands of identical PPU state, the
 * contract for drawing on the RSP (docs/n64/PPU_DESIGN.md, step 1). Built
 * only with N64PPU_DESC=1 (or the check build) until something on the N64
 * consumes it. */
#if defined(N64PPU_DESC_CHECK) && !defined(N64PPU_DESC)
#define N64PPU_DESC 1
#endif
#if N64PPU_DESC
void n64_ppu_desc_begin(const struct Ppu *ppu);
void n64_ppu_desc_line(const struct Ppu *ppu, int line);
void n64_ppu_desc_end(const struct Ppu *ppu);
#else
#define n64_ppu_desc_begin(ppu) ((void)0)
#define n64_ppu_desc_line(ppu, line) ((void)0)
#define n64_ppu_desc_end(ppu) ((void)0)
#endif

/* The beam driver's raster replay (snes_beam_frame_driver_draw_ppu_frame),
 * with each line expanded for the RDP and the software PPU run only where
 * needed. */
static void draw_frame_rdp(bool software_every_line) {
  snes_set_hdma_beam_enabled(g_snes, false);
  ppu_rasterRenderBegin(g_ppu);
  dma_initHdma(g_snes->dma);
  dma_doHdma(g_snes->dma);
  n64_ppu_build_begin(s_rdp_frame, s_pixels);
#ifdef N64PPU_DESC_CHECK
  { extern uint8_t *g_n64ppu_desc_live_pixels; extern size_t g_n64ppu_desc_live_pitch;
    g_n64ppu_desc_live_pixels = software_every_line ? g_ppu->renderBuffer : NULL;
    g_n64ppu_desc_live_pitch = g_ppu->renderPitch; }
#endif
  n64_ppu_desc_begin(g_ppu);
  for (int line = 1; line <= 224; line++) {
    uint8_t hdmaen;
    ppu_rasterApplyLine(g_ppu, line - 1);
    if (ppu_rasterTakeHdmaen(&hdmaen))
      dma_startDma(g_snes->dma, hdmaen, true);
    n64_ppu_desc_line(g_ppu, line);
    const bool rdp_ok = n64_ppu_build_line(g_ppu, line);
    if (software_every_line || !rdp_ok) ppu_runLine(g_ppu, line);
    else ppu_runLineState(g_ppu, line);
    dma_doHdma(g_snes->dma);
  }
  n64_ppu_build_end(g_ppu);
  n64_ppu_desc_end(g_ppu);
  ppu_rasterRenderEnd(g_ppu);
}

#if N64_RENDER
/* Finish a built frame (the RSP's background rows) and show it. */
#ifdef N64_PRESENT_PROF
static uint32_t s_pp_finish, s_pp_audio;
#endif
static void present_rdp_frame(uint32_t frame, const char *line) {
#ifdef N64_PRESENT_PROF
  const uint64_t pf = n64_platform_time_us();
  n64_ppu_build_finish();
  s_pp_finish += (uint32_t)(n64_platform_time_us() - pf);
#else
  n64_ppu_build_finish();
#endif
  const int diff = n64_ppu_draw_and_show(s_rdp_frame, line, N64_RENDER == 2);
#ifdef N64PPU_WHY_LOG
  { /* Software-PPU lines per reason (bits of N64PPU_WHY_*), every 60 frames. */
    static uint32_t s_sw, s_rdp, s_why_or;
    s_sw += s_rdp_frame->software_lines; s_rdp += s_rdp_frame->rdp_lines;
    s_why_or |= s_rdp_frame->fallback_reasons;
    if (frame % 60 == 59) {
      n64_platform_log("[ppuwhy] 60 frames: rdp lines %lu, software lines %lu, reasons %03lx\n",
                       (unsigned long)s_rdp, (unsigned long)s_sw, (unsigned long)s_why_or);
      s_sw = s_rdp = s_why_or = 0;
    }
  }
#endif
  static uint32_t s_diff_frames;   /* frames that differed (RENDER=2) */
  if (diff > 0) s_diff_frames++;
  if (N64_RENDER == 2 && (frame < 8 || frame % 30 == 0 || diff > 0))
    if (frame < 600 || frame % 30 == 0 || s_diff_frames <= 20)
      n64_platform_log("[rdp] frame %lu: rdp %d sw %d black %d lines, why %02lx, diff %d px\n",
                       (unsigned long)frame, s_rdp_frame->rdp_lines,
                       s_rdp_frame->software_lines, s_rdp_frame->black_lines,
                       (unsigned long)s_rdp_frame->fallback_reasons, diff);
#if N64PPU_SCENE
  { /* Scene sprites: lines drawn from strips / from planes, rectangles. */
    extern uint32_t g_scene_obj_lines, g_scene_obj_plane_lines, g_scene_obj_rects, g_scene_mode_passes[4];
    if (N64_RENDER == 2 && frame % 30 == 0) {
      extern uint32_t g_scene_sync_blocks;
      n64_platform_log("[scene] BG passes: plain %lu, clipped %lu, subscreen %lu, subscreen clipped %lu; "
                       "VRAM blocks synced %lu\n",
                       (unsigned long)g_scene_mode_passes[0], (unsigned long)g_scene_mode_passes[1],
                       (unsigned long)g_scene_mode_passes[2], (unsigned long)g_scene_mode_passes[3],
                       (unsigned long)g_scene_sync_blocks);
      g_scene_sync_blocks = 0;
      memset(g_scene_mode_passes, 0, sizeof g_scene_mode_passes);
    }
    if (N64_RENDER == 2 && frame % 30 == 0)
      n64_platform_log("[scene] obj lines: scene %lu, planes %lu, rects %lu (last 30 frames); "
                       "frames differing so far %lu\n",
                       (unsigned long)g_scene_obj_lines, (unsigned long)g_scene_obj_plane_lines,
                       (unsigned long)g_scene_obj_rects, (unsigned long)s_diff_frames);
    if (frame % 30 == 0) g_scene_obj_lines = g_scene_obj_plane_lines = g_scene_obj_rects = 0; }
#endif
}
#endif

/* N64_DSP_CLOCK (with -DSNESRECOMP_DSP_CYCLE_HOOK=n64_dsp_cycle_timed):
 * times each S-DSP sample twice with the CPU cycle counter, first as it
 * runs (cold cache), then again from a restored copy of its state (warm),
 * and shows both on the status line. State afterwards is as if dsp_cycle
 * ran once. */
#ifdef N64_DSP_CLOCK
#include <stddef.h>
#include "snes/dsp.h"
static uint32_t s_dsp_n, s_dsp_cold, s_dsp_warm, s_dsp_show_cold, s_dsp_show_warm;
static inline uint32_t cycle_count(void) {
  uint32_t c;
  __asm__ volatile("mfc0 %0, $9" : "=r"(c));
  return c * 2u;  /* COP0 Count runs at half the CPU clock */
}
void n64_dsp_cycle_timed(Dsp *d) {
  static uint8_t save[sizeof(Dsp)];
  const size_t off = offsetof(Dsp, ram);
  const size_t len = offsetof(Dsp, sampleBuffer) - offsetof(Dsp, ram);
  const uint16_t adr = d->echoBufferAdr + d->echoBufferIndex * 4;
  uint8_t echo[4];
  for (int i = 0; i < 4; ++i) echo[i] = d->apu_ram[(uint16_t)(adr + i)];
  memcpy(save, (uint8_t *)d + off, len);
  const uint32_t w = d->sampleWrite;
  uint32_t t0 = cycle_count(); dsp_cycle(d); uint32_t t1 = cycle_count();
  memcpy((uint8_t *)d + off, save, len);
  d->sampleWrite = w;
  for (int i = 0; i < 4; ++i) d->apu_ram[(uint16_t)(adr + i)] = echo[i];
  uint32_t t2 = cycle_count(); dsp_cycle(d); uint32_t t3 = cycle_count();
  s_dsp_n++; s_dsp_cold += t1 - t0; s_dsp_warm += t3 - t2;
  if (s_dsp_n == 4096) {
    s_dsp_show_cold = s_dsp_cold / s_dsp_n; s_dsp_show_warm = s_dsp_warm / s_dsp_n;
    n64_platform_log("[dsp] cycles per sample: cold %lu warm %lu\n",
                     (unsigned long)s_dsp_show_cold, (unsigned long)s_dsp_show_warm);
    s_dsp_n = s_dsp_cold = s_dsp_warm = 0;
  }
}
#endif

/* ---- Host contract (the hooks common_rtl.c calls) -------------------- */

struct SpcPlayer *g_spc_player;
bool g_new_ppu = true;
bool g_ws_active = false;
int g_ws_extra = 0;

void NORETURN Die(const char *error) {
  n64_platform_fatal(error ? error : "(null)");
}

/* Single-threaded host: the APU is only touched from the guest thread. */
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

void MkDir(const char *s) { (void)s; }
void ChangeWindowScale(int scale_step) { (void)scale_step; }

/* The N64 host presents from the PPU's own buffer (see present_frame), so
 * the desktop presentation path is unused. */
void RtlDrawPpuFrame(uint8 *pixel_buffer, size_t pitch, uint32 render_flags) {
  (void)pixel_buffer; (void)pitch; (void)render_flags;
}

/* Trace hooks: no-ops outside the trace build (same as host_contract.c in a
 * scaffolded desktop project). */
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y) {
  (void)pc; (void)a; (void)x; (void)y;
}
void debug_on_wram_write_byte(uint32_t addr, uint8_t old_val, uint8_t new_val) {
  (void)addr; (void)old_val; (void)new_val;
}
void debug_on_wram_write_word(uint32_t addr, uint16_t old_val, uint16_t new_val) {
  (void)addr; (void)old_val; (void)new_val;
}

/* ---- Game glue: the framework's generic beam-aligned driver ---------- */

static void n64_run_frame(void) { snes_beam_frame_driver_run_frame(); }
static void n64_draw_ppu_frame(void) { snes_beam_frame_driver_draw_ppu_frame(); }
static void n64_reset(void) { snes_beam_frame_driver_reset(); }

static const RtlGameInfo kN64GameInfo = {
    .title = "n64-interp",
    .initialize = NULL,
    .run_frame = &n64_run_frame,
    .draw_ppu_frame = &n64_draw_ppu_frame,
    .save_name_prefix = "save",
    .session_reset = &n64_reset,
    .hardware_reset = &n64_reset,
};

/* ---- Input ----------------------------------------------------------- */

/* ---- Video ----------------------------------------------------------- */

enum { kSnesW = 256, kSnesH = 224 };
static uint32_t *s_pixels;  /* PPU output, XRGB8888, kSnesW x kSnesH */

/* ---- Audio ----------------------------------------------------------- */

static void render_audio(int16_t *stereo, int frames) {
  RtlRenderAudio(stereo, frames, 2);
}

/* ---- Heap colour --------------------------------------------------------- */

/* The engine's large heap objects (PPU, APU, WRAM-side state, ...) come from
 * the top of the heap; small ones may fill earlier holes. Pad the heap top
 * to a fixed colour modulo the 8 KB data cache before SnesInit, so a change
 * in what the host allocated earlier (the RDP frame description, buffers)
 * cannot move them all to new cache conflicts. Measured 2026-11-09: a 3 KB
 * larger host allocation made the CPU stage 17% slower. N64_HEAP_COLOR -1
 * only logs the colour the top has. */
#ifndef N64_HEAP_COLOR
#define N64_HEAP_COLOR 0x1998   /* SMW attract, trace sweep 2026-11-09 */
#endif
#if defined(__mips__)
#include <malloc.h>
#include <unistd.h>

#ifdef N64_STATE_AT_PC
/* Guest sync-point check: -DN64_STATE_AT_PC=0xBBAAAA. Each time the
 * interpreter is about to execute that PC, hash WRAM and the registers.
 * Unlike [state] (taken at host frame ends, where compiled code stops a
 * block later than interpreted code), this samples the guest at the same
 * point of its own program in every build, so two builds that run the same
 * guest computation print the same lines whatever is compiled. Pick a PC
 * the top-level loop passes once per game frame. */
void n64_state_at_pc(void) {
  static uint32_t s_hits;
  uint32_t h = 2166136261u;
  for (int i = 0; i < 0x20000; ++i) h = (h ^ g_snes->ram[i]) * 16777619u;
  n64_platform_log("[sync] %lu wram %08lx a %04x x %04x y %04x s %04x p %02x\n",
                   (unsigned long)s_hits++, (unsigned long)h, cpu_A(), cpu_X(), cpu_Y(), cpu_S(),
                   cpu_P());
}
#endif
static void *volatile s_heap_pad;
/* Where the next block from the top of the heap starts: the top chunk is
 * the last mallinfo().keepcost bytes below the break, and a block's data
 * follows its 8-byte header. (A probe allocation would be served from any
 * large enough hole instead.) */
static uintptr_t heap_top(void) {
  const struct mallinfo mi = mallinfo();
  return (uintptr_t)sbrk(0) - (uintptr_t)mi.keepcost + 8;
}
static void heap_anchor(long color) {
  const uintptr_t top = heap_top();
  if (color >= 0) {
    uintptr_t d = ((uintptr_t)color - top) & 8191;
    if (d && d < 32) d += 8192;
    /* The pad must come from the top: a smaller free hole would serve a
     * small request. Grow it in 8 KB steps (same colour) until it does. */
    for (int k = 0; d && k < 64; ++k, d += 8192) {
      void *p = malloc(d - 8);   /* chunk = request + header, 8-aligned */
      if ((uintptr_t)p == top) { s_heap_pad = p; break; }
      free(p);
    }
  }
  n64_platform_log("[heap] top colour %04lx -> %04lx (want %ld)\n", (unsigned long)(top & 8191),
                   (unsigned long)(heap_top() & 8191), color);
}
#endif

/* ---- ROM ------------------------------------------------------------- */

#ifndef N64_ROM_PATH
#define N64_ROM_PATH "rom:/game.sfc"
#endif

static uint8_t *load_rom(const char *path, int *size_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = malloc((size_t)size);
  if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
    free(data);
    data = NULL;
  }
  fclose(f);
  /* Strip a 512-byte copier header if present. */
  if (data && (size & 0x3ff) == 0x200) {
    memmove(data, data + 0x200, (size_t)size - 0x200);
    size -= 0x200;
  }
  *size_out = (int)size;
  return data;
}

/* ---- Main loop ------------------------------------------------------- */

/* The main loop runs on its own stack, placed so the stack's hot lines
 * fall in data-cache sets the rest of the runtime uses least. The VR4300
 * data cache is 8 KB and direct-mapped; libdragon's default stack top (the
 * end of RDRAM, 0 mod 8 KB) put the deepest hot frames on the same lines as
 * the PPU, sound and interpreter state. N64_STACK_COLOR is the stack top
 * modulo 8 KB, chosen from a data-cache miss profile (see the notes);
 * -1 keeps libdragon's stack. */
#ifndef N64_STACK_COLOR
#ifdef N64_RSPDSP
#define N64_STACK_COLOR 0x1820  /* trace sweep 2026-11-09 (with data_order.ld) */
#else
#define N64_STACK_COLOR 0x11e0
#endif
#endif
#ifdef N64_DCPROF
static volatile uint32_t s_dcprof_start = N64_DCPROF, s_dcprof_stop = N64_DCPROF + 60;
#else
static volatile uint32_t s_dcprof_start = 0xffffffffu, s_dcprof_stop = 0xffffffffu;
#endif
static int host_main(void);
#if N64_STACK_COLOR >= 0 && defined(__mips__)
static uint8_t s_host_stack[256 * 1024] __attribute__((aligned(8192)));
#if SNESRECOMP_BLUE_THREAD
/* The guest runs on its own stack (blue_thread.h); its frames are the hot
 * ones, so it gets the placement the main stack was tuned for. */
static uint8_t s_blue_stack[256 * 1024] __attribute__((aligned(8192)));
#endif
#endif
int main(void) {
#if N64_STACK_COLOR >= 0 && defined(__mips__)
  /* host_main never returns. Leave a small argument area above sp. */
  uintptr_t top = (uintptr_t)s_host_stack + sizeof s_host_stack - 8192 + (N64_STACK_COLOR & 8191);
  __asm__ volatile("move $sp, %0\n\tjal %1\n\tnop" :: "r"(top - 64), "i"(host_main) : "memory");
  __builtin_unreachable();
#else
  return host_main();
#endif
}

/* Code cache (N64_CODE_CACHE=1, n64/Makefile CODECACHE=1): compiled routine
 * bodies are modules rom:/cc/<name>.dso, loaded on first call and unloaded
 * to stay within N64_CODE_CACHE_BYTES (runner/src/recomp_cache.h). */
#if N64_CODE_CACHE
#include "recomp_cache.h"
static uint64_t s_cc_load_us, s_cc_load_max_us;   /* time spent loading modules */
static void code_cache_load(RecompRoutine *rt) {
  const uint64_t t0 = n64_platform_time_us();
  void *base = NULL;
  rt->fn = (RecompFn)n64_platform_codepack_load(rt->id, &base);
  if (!rt->fn) Die("Code cache: out of memory loading a routine.");
  rt->host = base;
  const uint64_t dt = n64_platform_time_us() - t0;
  s_cc_load_us += dt;
  if (dt > s_cc_load_max_us) s_cc_load_max_us = dt;
}
static void code_cache_unload(RecompRoutine *rt) {
  n64_platform_codepack_free(rt->host);
  rt->host = NULL;
}
/* Background loading (N64_CODE_CACHE_ASYNC, make CODECACHE_ASYNC=1,
 * experimental): a call to an absent routine starts its DMA and runs the
 * routine in the interpreter this time (recomp_cache_interp_fallback), so no
 * frame waits for the cartridge. */
#ifndef N64_CODE_CACHE_ASYNC
#define N64_CODE_CACHE_ASYNC 0
#endif
#if N64_CODE_CACHE_ASYNC
#define S_CC_BEGIN_US s_cc_begin_us
static uint64_t s_cc_begin_us, s_cc_begin_last_us;
static int code_cache_begin(RecompRoutine *rt) {
  if (g_recomp_fallback[rt->id].pc24 == 0xFFFFFFFFu) {   /* must load first */
    code_cache_load(rt);
    return 0;
  }
  const uint64_t t0 = n64_platform_time_us();
  void *base = NULL;
  if (!n64_platform_codepack_begin(rt->id, &base))
    Die("Code cache: out of memory loading a routine.");
  rt->host = base;
  s_cc_begin_last_us = n64_platform_time_us() - t0;
  s_cc_begin_us += s_cc_begin_last_us;
  return 1;
}
static int code_cache_finish(RecompRoutine *rt, int wait) {
  if (rt->fn) return 1;                 /* loaded synchronously in begin */
  const uint64_t t0 = n64_platform_time_us();
  void *entry = n64_platform_codepack_finish(wait);
  const uint64_t dt = n64_platform_time_us() - t0;
  s_cc_load_us += dt;
  if (!entry) return 0;
  if (dt + s_cc_begin_last_us > s_cc_load_max_us) s_cc_load_max_us = dt + s_cc_begin_last_us;
  rt->fn = (RecompFn)entry;
  return 1;
}
#else
#define S_CC_BEGIN_US 0
#endif
static void code_cache_size(uint32_t id, uint32_t bytes) {
  if (id < g_recomp_routine_count) g_recomp_routines[id]->size = bytes;
}
static void code_cache_init(void) {
  for (uint32_t i = 0; i < g_recomp_routine_count; i++) g_recomp_routines[i]->id = i;
  const int n = n64_platform_codepack_open("rom:/cc.pack", code_cache_size);
  if (n != (int)g_recomp_routine_count)
    Die("Code cache: rom:/cc.pack is missing or does not match this build.");
  recomp_cache_set_loader(code_cache_load, code_cache_unload);
#if N64_CODE_CACHE_ASYNC
  recomp_cache_set_async(code_cache_begin, code_cache_finish, recomp_cache_interp_fallback);
#endif
  recomp_cache_set_capacity(N64_CODE_CACHE_BYTES);
  n64_platform_log("[n64] code cache: %u routines, %u KB budget\n",
                   g_recomp_routine_count, (unsigned)(N64_CODE_CACHE_BYTES / 1024));
}
#endif

/* ROM paging (N64_ROM_PAGING=1): N64_ROM_PAGE_SLOTS slots of 8 KB. */
#ifndef N64_ROM_PAGING
#define N64_ROM_PAGING 0
#endif
#ifndef N64_ROM_PAGE_SLOTS
#define N64_ROM_PAGE_SLOTS 24
#endif
static uint8_t *s_rom_window;
#if N64_ROM_PAGING
static uint8_t *rom_window_provider(const uint8_t *data, int length) {
  (void)length;
  return data == s_rom_window ? s_rom_window : NULL;
}
#endif

#ifdef N64_PHASE0
static char s_p0_line[40];   /* Red sizing summary for the status line */
#endif

static int host_main(void) {
#if SNESRECOMP_BLUE_THREAD
  run_flags_reset();   /* before any interrupt can set a flag */
#endif
#if N64_STACK_COLOR >= 0 && defined(__mips__) && SNESRECOMP_BLUE_THREAD
  blue_set_stack(s_blue_stack, s_blue_stack + sizeof s_blue_stack - 8192 +
                                           (N64_STACK_COLOR & 8191) - 64);
#endif
  n64_platform_init();
  if (!n64_platform_has_expansion_pak())
    Die("This build needs the Expansion Pak (8 MB).");
#ifdef N64_SPC_DBT_TEST
  { extern void spc_dbt_test(void); spc_dbt_test(); }   /* (does not return) */
#endif

  int rom_size = 0;
  uint8_t *rom = NULL;
#if N64_ROM_PAGING
  /* ROM paging: the cartridge reads its ROM through a window that pages
   * from the cartridge on demand (n64_platform_rom_window), so no copy of
   * the ROM is kept in RAM. */
  rom = s_rom_window = n64_platform_rom_window(N64_ROM_PATH, &rom_size, N64_ROM_PAGE_SLOTS);
  if (rom) {
    cart_set_rom_provider(rom_window_provider);
    n64_platform_log("[n64] ROM paged: %d bytes, %d slots of %d KB\n", rom_size,
                     N64_ROM_PAGE_SLOTS, N64_ROM_PAGE_BYTES / 1024);
  }
#endif
  if (!rom) {
    rom = load_rom(N64_ROM_PATH, &rom_size);
    if (!rom) Die("Could not read rom:/game.sfc. Rebuild with ROM=path/to/game.sfc.");
    n64_platform_log("[n64] ROM loaded: %d bytes\n", rom_size);
  }

  s_pixels = calloc(kSnesW * kSnesH, sizeof(uint32_t));
  if (!s_pixels) Die("Out of memory for the PPU frame buffer.");
#if N64_RENDER
  s_rdp_frame = n64_ppu_build_frame_alloc();
  if (!s_rdp_frame) Die("Out of memory for the RDP frame planes.");
#endif

#if N64_CODE_CACHE
  code_cache_init();
#endif
  RtlSetAudioOutputRate(n64_platform_audio_init(32000));
  RtlRegisterGame(&kN64GameInfo);
#if defined(__mips__)
  /* Place the engine's heap objects at a fixed data-cache colour, so what
   * the host allocated before (frame description, buffers) cannot move
   * them. Measured 2026-11-09: a 3 KB larger host allocation made the CPU
   * stage 17% slower through new cache conflicts between them. */
  heap_anchor(N64_HEAP_COLOR);
#endif
  if (!SnesInit(rom, rom_size)) Die("SnesInit failed: unsupported or damaged ROM.");
  if (rom != s_rom_window) free(rom);  /* else the cartridge reads the window */
  n64_platform_log("[heap] snes %p ppu %p apu %p cart rom %p\n", (void *)g_snes, (void *)g_ppu,
                   (void *)g_snes->apu, (void *)g_snes->cart->rom);
  /* All OAM changes here go through the PPU ports or a save-state load. */
  ppu_set_oam_write_tracking(g_ppu);
  n64_platform_log("[n64] SNES initialized, entering main loop\n");

  uint32_t frame = 0;
  uint64_t acc_run = 0, acc_draw = 0, acc_present = 0;
#if N64_PIPELINE
  /* The frame built last iteration is drawn after this iteration's
   * emulation, so the RSP expands its backgrounds while the CPU runs. */
  bool pending = false;
  uint32_t pending_frame = 0;
  char pending_line[64] = "";
#endif
  for (;;) {
    uint32_t inputs = n64_platform_read_pad();

    uint64_t t0 = n64_platform_time_us();
    n64_platform_set_phase(N64_PHASE_CPU);
#ifdef N64_HANG_PROBE
    { extern volatile uint32_t g_hang_where, g_hang_frame; g_hang_frame = frame; g_hang_where = 0x01; }
#define HOST_WHERE(v) do { extern volatile uint32_t g_hang_where; g_hang_where = (v); } while (0)
#else
#define HOST_WHERE(v) ((void)0)
#endif
#if N64_RENDER
    n64_ppu_build_release_vram();   /* the RSP has its VRAM snapshot */
#endif
#if N64_CODE_CACHE
    recomp_cache_poll();   /* background code-cache load (nothing to do without one) */
#endif
    RtlRunFrame(inputs);
    HOST_WHERE(0x02);
    uint64_t t1 = n64_platform_time_us();
    n64_platform_set_phase(N64_PHASE_PRESENT);
#if N64_PIPELINE
    if (pending) present_rdp_frame(pending_frame, pending_line);
#endif
    uint64_t t1p = n64_platform_time_us();
    HOST_WHERE(0x03);
    n64_platform_set_phase(N64_PHASE_PPU);
    PpuBeginDrawing(g_ppu, (uint8_t *)s_pixels, kSnesW * 4, kPpuRenderFlags_NewRenderer);
#if N64_RENDER
    draw_frame_rdp(N64_RENDER == 2);
#else
    n64_draw_ppu_frame();
#endif
    uint64_t t2 = n64_platform_time_us();
    HOST_WHERE(0x04);
    n64_platform_set_phase(N64_PHASE_PRESENT);

    char line[64];
    snprintf(line, sizeof line, "f%lu cpu %lums ppu %lums",
             (unsigned long)frame, (unsigned long)((t1 - t0) / 1000),
             (unsigned long)((t2 - t1p) / 1000));
#ifdef N64_PHASE0
    /* Red sizing on screen (the debug log needs a USB link on hardware):
     * q queue words, w CPU queue-buffer wait (us), i RSP idle %, p APU
     * port writes / most between SPC runs, per frame over the last 60. */
    if (s_p0_line[0]) snprintf(line, sizeof line, "%s f%lu", s_p0_line, (unsigned long)frame);
#endif
#ifdef N64_DSP_CLOCK
    snprintf(line, sizeof line, "f%lu cpu %lu ppu %lu dsp %lu/%lu",
             (unsigned long)frame, (unsigned long)((t1 - t0) / 1000),
             (unsigned long)((t2 - t1p) / 1000), (unsigned long)s_dsp_show_cold,
             (unsigned long)s_dsp_show_warm);
#endif
#if N64_PIPELINE
    pending = true;
    pending_frame = frame;
    memcpy(pending_line, line, sizeof line);
#elif N64_RENDER
    present_rdp_frame(frame, line);
#else
    n64_platform_present(s_pixels, kSnesW, kSnesH, line);
#endif
#ifdef N64_PRESENT_PROF
    { const uint64_t pa = n64_platform_time_us();
      n64_platform_audio_pump(render_audio);
      s_pp_audio += (uint32_t)(n64_platform_time_us() - pa); }
    if (frame % 60 == 59) {
      extern uint32_t g_present_prof[5];
      n64_platform_log("[present] per frame (us): RSP BG merge %lu, display_get %lu, RDP commands %lu, "
                       "status line %lu, RDP wait %lu, display_show %lu, audio pump %lu\n",
                       (unsigned long)(s_pp_finish / 60), (unsigned long)(g_present_prof[0] / 60 / 47),
                       (unsigned long)(g_present_prof[1] / 60 / 47), (unsigned long)(g_present_prof[2] / 60 / 47),
                       (unsigned long)(g_present_prof[3] / 60 / 47), (unsigned long)(g_present_prof[4] / 60 / 47),
                       (unsigned long)(s_pp_audio / 60));
      { extern uint32_t g_present_split[5], g_present_rspw[4];
        n64_platform_log("[present] RDP commands split (us): scene BG %lu, scene OBJ %lu, planes %lu, "
                         "detach wait %lu, RSP queue-buffer waits %lu; RSP drained: before %lu, BG %lu, "
                         "OBJ %lu, planes/rest %lu\n",
                         (unsigned long)(g_present_split[0] / 60 / 47), (unsigned long)(g_present_split[1] / 60 / 47),
                         (unsigned long)(g_present_split[2] / 60 / 47), (unsigned long)(g_present_split[3] / 60 / 47),
                         (unsigned long)(g_present_split[4] / 60 / 47), (unsigned long)(g_present_rspw[0] / 60 / 47),
                         (unsigned long)(g_present_rspw[1] / 60 / 47), (unsigned long)(g_present_rspw[2] / 60 / 47),
                         (unsigned long)(g_present_rspw[3] / 60 / 47));
        memset(g_present_split, 0, sizeof(uint32_t) * 5);
        memset(g_present_rspw, 0, sizeof(uint32_t) * 4); }
#if N64PPU_SCENE
      { extern uint32_t g_scene_ticks, g_scene_tlut_ticks, g_scene_sync_ticks, g_scene_passes;
#if N64PPU_SCENE_RSP
        extern uint32_t n64_scene_rsp_room_waits(void);
        n64_platform_log("[scene] scene commands retried for RDP room: %lu (total)\n",
                         (unsigned long)n64_scene_rsp_room_waits());
#endif
        n64_platform_log("[scene] per frame (us): passes %lu, scene_draw_bg %lu (TLUTs %lu), cache sync %lu\n",
                         (unsigned long)(g_scene_passes / 60), (unsigned long)(g_scene_ticks / 60 / 47),
                         (unsigned long)(g_scene_tlut_ticks / 60 / 47), (unsigned long)(g_scene_sync_ticks / 60 / 47));
        g_scene_ticks = g_scene_tlut_ticks = g_scene_sync_ticks = g_scene_passes = 0; }
#endif
      { /* On screen too (hardware has no log without USB), tenths of a ms:
         * c commands, b BG wait, a audio, p RDP pipe busy, t TMEM busy. */
        extern uint32_t g_present_rdp[3];
        extern char g_present_line[48];
        n64_platform_log("[present] RDP per frame (us): pipe busy %lu, TMEM busy %lu, command buffer busy %lu\n",
                         (unsigned long)(g_present_rdp[0] / 60 / 62), (unsigned long)(g_present_rdp[1] / 60 / 62),
                         (unsigned long)(g_present_rdp[2] / 60 / 62));
        int o = snprintf(g_present_line, 48, "c%lu b%lu a%lu p%lu t%lu",
                 (unsigned long)(g_present_prof[1] / 60 / 4700), (unsigned long)(s_pp_finish / 60 / 100),
                 (unsigned long)(s_pp_audio / 60 / 100), (unsigned long)(g_present_rdp[0] / 60 / 6250),
                 (unsigned long)(g_present_rdp[1] / 60 / 6250));
#if N64PPU_DESC
        { /* the frame description (step 1): bands in the last frame */
          extern const struct N64PpuDesc *n64_ppu_desc(void);
          extern int n64_ppu_desc_bands(void);
          o += snprintf(g_present_line + o, 48 - o, " n%d", n64_ppu_desc_bands()); }
#endif
        { /* ring words rewritten after a failed read-back (rspq_ring_write) */
          /* r lost, D of them with an RSP DMA busy, S SP_STATUS before/after the
           * last one, B% of all stores made with a DMA busy (the baseline) */
          extern uint32_t rspq_ring_readback_bad, rspq_ring_lost_dma, rspq_ring_lost_status[2];
          extern uint32_t rspq_ring_stores, rspq_ring_stores_dma;
          #if N64PPU_SCENE_RSP
          { extern uint32_t n64_scene_rsp_room_waits(void);
            /* r lost DMEM stores, q scene commands retried for RDP room */
            snprintf(g_present_line + o, 48 - o, " r%lu q%lu", (unsigned long)rspq_ring_readback_bad,
                     (unsigned long)n64_scene_rsp_room_waits()); }
#else
          snprintf(g_present_line + o, 48 - o, " r%lu D%lu S%lx/%lx B%lu", (unsigned long)rspq_ring_readback_bad,
                   (unsigned long)rspq_ring_lost_dma, (unsigned long)(rspq_ring_lost_status[0] & 0x1f),
                   (unsigned long)(rspq_ring_lost_status[1] & 0x1f),
                   (unsigned long)(rspq_ring_stores ? (uint64_t)rspq_ring_stores_dma * 100 / rspq_ring_stores : 0));
#endif
        }
        for (int i = 0; i < 3; i++) g_present_rdp[i] = 0;
      }
      s_pp_finish = s_pp_audio = 0;
      for (int i = 0; i < 5; i++) g_present_prof[i] = 0;
    }
#else
    n64_platform_audio_pump(render_audio);
#endif
    uint64_t t3 = n64_platform_time_us() + (t1p - t1);   /* present: both parts */

#ifdef N64_TIME_CATS
    { /* Exclusive time by category over the frame (time_cats.h), in us. */
      extern uint64_t g_tcat_acc[];
      static uint64_t s_tc_sum[7];
      static uint32_t s_tc_it, s_tc_op;
      static uint32_t s_tc_frames;
      (void)tcat_enter(TCAT_OTHER);
      for (int k = 0; k < 7; k++) { s_tc_sum[k] += g_tcat_acc[k]; g_tcat_acc[k] = 0; }
      s_tc_it += g_tcat_iters; s_tc_op += g_tcat_ops; g_tcat_iters = g_tcat_ops = 0;
      { extern unsigned long long g_interp_bridge_bounces; static unsigned long long s_b0;
        if (frame >= 360 && s_tc_frames == 59)
          n64_platform_log("[tcat] bounces into compiled bodies: %lu per frame\n",
                           (unsigned long)((g_interp_bridge_bounces - s_b0) / 60));
        if (s_tc_frames == 59 || frame < 360) s_b0 = g_interp_bridge_bounces; }
      if (frame >= 360 && ++s_tc_frames == 60) {
        n64_platform_log("[tcat] frame %lu, per frame: interp %lu us, compiled %lu us, spc %lu us, io %lu us, "
                         "beam %lu us, other %lu us; opcodes %lu us (%lu ops, %lu loop iterations)\n", (unsigned long)frame,
                         (unsigned long)(s_tc_sum[1] * 8 / 375 / 60), (unsigned long)(s_tc_sum[2] * 8 / 375 / 60),
                         (unsigned long)(s_tc_sum[3] * 8 / 375 / 60), (unsigned long)(s_tc_sum[4] * 8 / 375 / 60),
                         (unsigned long)(s_tc_sum[5] * 8 / 375 / 60), (unsigned long)(s_tc_sum[0] * 8 / 375 / 60),
                         (unsigned long)(s_tc_sum[6] * 8 / 375 / 60), (unsigned long)(s_tc_op / 60),
                         (unsigned long)(s_tc_it / 60));
        s_tc_frames = 0;
      }
      if (s_tc_frames == 0) { for (int k = 0; k < 7; k++) s_tc_sum[k] = 0; s_tc_it = s_tc_op = 0; }
    }
#endif
    if (frame < 8 || frame % 50 == 0)
      n64_platform_log("[n64] frame %lu: run %lu us, ppu %lu us\n", (unsigned long)frame,
                       (unsigned long)(t1 - t0), (unsigned long)(t2 - t1p));
#ifdef SNESRECOMP_APU_TIME
    { /* SPC catch-up time in this frame (APUTIME=1 builds). */
      extern uint32_t n64_platform_apu_us(void);
      static uint32_t s_apu_last;
      const uint32_t now = n64_platform_apu_us();
      if (frame % 50 == 0)
        n64_platform_log("[aputime] frame %lu: %lu us\n", (unsigned long)frame, (unsigned long)(now - s_apu_last));
      s_apu_last = now;
    }
#endif
    acc_run += t1 - t0;
    acc_draw += t2 - t1p;
    acc_present += t3 - t2;
#ifdef N64_PROFILE_FRAMES
    if (frame == N64_PROFILE_START) n64_platform_profile_start(N64_PROFILE_US);
    if (frame == N64_PROFILE_START + N64_PROFILE_FRAMES) {
      n64_platform_profile_stop_and_dump();
#if N64_CODE_CACHE
      /* Where the resident compiled routines are, so symbolize.py can name
       * samples in code-cache modules: "PROFMOD <base> <bytes> <name>". */
      for (unsigned i = 0; i < g_recomp_routine_count; ++i) {
        const RecompRoutine *rt = g_recomp_routines[i];
        if (rt->fn && rt->host)
          n64_platform_log("PROFMOD %08lx %lu %s\n", (unsigned long)(uintptr_t)rt->host,
                 (unsigned long)rt->size, rt->name);
      }
#endif
    }
#endif
    /* Data-cache miss profile window for ares's local profiler (see
     * n64/profile/dcache_report.py): heap objects first, then markers.
     * Always compiled in; N64_DCPROF=<frame> only changes the two data
     * words below, so a profiling build has the shipped memory layout. */
    if (frame == s_dcprof_start) {
      Snes *sn = g_snes;
      n64_platform_log("[heap] snes %08lx %lu\n", (unsigned long)(uintptr_t)sn, (unsigned long)sizeof(*sn));
      n64_platform_log("[heap] wram %08lx %lu\n", (unsigned long)(uintptr_t)sn->ram, 0x20000ul);
      n64_platform_log("[heap] apu %08lx %lu\n", (unsigned long)(uintptr_t)sn->apu, (unsigned long)sizeof(*sn->apu));
      n64_platform_log("[heap] dsp %08lx %lu\n", (unsigned long)(uintptr_t)sn->apu->dsp, (unsigned long)sizeof(*sn->apu->dsp));
      n64_platform_log("[heap] spc %08lx %lu\n", (unsigned long)(uintptr_t)sn->apu->spc, (unsigned long)sizeof(*sn->apu->spc));
      n64_platform_log("[heap] ppu %08lx %lu\n", (unsigned long)(uintptr_t)g_ppu, (unsigned long)sizeof(*g_ppu));
      n64_platform_log("[heap] cart_rom %08lx %lu\n", (unsigned long)(uintptr_t)sn->cart->rom, (unsigned long)sn->cart->romSize);
      if (sn->cart->ram)
        n64_platform_log("[heap] cart_sram %08lx %lu\n", (unsigned long)(uintptr_t)sn->cart->ram, (unsigned long)sn->cart->ramSize);
#if N64_RENDER
      n64_platform_log("[heap] rdp_frame %08lx %lu\n", (unsigned long)(uintptr_t)s_rdp_frame, (unsigned long)sizeof(*s_rdp_frame));
      for (int p = 0; p < N64PPU_PLANES; ++p)
        n64_platform_log("[heap] rdp_plane%d %08lx %lu\n", p, (unsigned long)(uintptr_t)s_rdp_frame->plane[p],
                         (unsigned long)N64PPU_STRIDE * N64PPU_LINES);
#endif
      n64_platform_log("[heap] sw_pixels %08lx %lu\n", (unsigned long)(uintptr_t)s_pixels, (unsigned long)(kSnesW * kSnesH * 4));
      n64_platform_log("DCPROF_START\n");
    }
    if (frame == s_dcprof_stop) n64_platform_log("DCPROF_STOP\n");
#ifdef N64_STATE_HASH
    {
      /* Every DSP output sample, as produced (the frame end has synced a
       * deferred DSP), so builds with different DSP paths compare. */
      static uint32_t s_out_hash = 2166136261u, s_out_pos;
#if SNESRECOMP_DSP_STREAM
      /* The S-DSP's ring, to the SPC's time (hash builds only wait here). */
      apu_dspFrameSync(g_snes->apu);
      apu_yellow_wait(g_snes->apu->dspEdges);
      apu_dspOutput(g_snes->apu);
      const Dsp *d = g_snes->apu->dsp;
      for (; d->outRing && s_out_pos != d->sampleWrite; ++s_out_pos) {
        const int16_t *p = d->outRing + 4u * (s_out_pos & d->outMask);
        s_out_hash = (s_out_hash ^ (uint16_t)p[0]) * 16777619u;
        s_out_hash = (s_out_hash ^ (uint16_t)p[1]) * 16777619u;
        if (!((s_out_pos + 1) & 0xffff))   /* at fixed counts: independent of the SPC's lead */
          n64_platform_log("[dspmark] %lu %08lx\n", (unsigned long)(s_out_pos + 1), (unsigned long)s_out_hash);
      }
#else
      apu_dspSync(g_snes->apu);  /* all samples so far, also with async batches */
      const Dsp *d = g_snes->apu->dsp;
      for (; s_out_pos != d->sampleWrite; ++s_out_pos) {
        const uint32_t w = s_out_pos & (DSP_SAMPLE_RING - 1);
        s_out_hash = (s_out_hash ^ (uint16_t)d->sampleBuffer[w * 2]) * 16777619u;
        s_out_hash = (s_out_hash ^ (uint16_t)d->sampleBuffer[w * 2 + 1]) * 16777619u;
        if (!((s_out_pos + 1) & 0xffff))
          n64_platform_log("[dspmark] %lu %08lx\n", (unsigned long)(s_out_pos + 1), (unsigned long)s_out_hash);
      }
#endif
#ifndef N64_STATE_HASH_PERIOD
#define N64_STATE_HASH_PERIOD 60
#endif
#ifdef N64_STATE_PAGES_FROM
      /* Localise a difference between two builds: for each frame in the
       * window, a 16-bit hash of every 256-byte WRAM page (64 per line) and
       * the registers. -DN64_STATE_PAGES_FROM=a -DN64_STATE_PAGES_TO=b. */
      if (frame >= N64_STATE_PAGES_FROM && frame <= N64_STATE_PAGES_TO) {
        static const char hex[] = "0123456789abcdef";
        char line[64 * 4 + 1];
        for (int row = 0; row < 8; ++row) {
          for (int pg = 0; pg < 64; ++pg) {
            uint32_t ph = 2166136261u;
            const uint8_t *p = g_snes->ram + (row * 64 + pg) * 256;
            for (int i = 0; i < 256; ++i) ph = (ph ^ p[i]) * 16777619u;
            ph ^= ph >> 16;
            for (int k = 0; k < 4; ++k) line[pg * 4 + k] = hex[(ph >> (12 - 4 * k)) & 15];
          }
          line[64 * 4] = 0;
          n64_platform_log("[pages] %lu %d %s\n", (unsigned long)frame, row, line);
        }
        n64_platform_log("[regs] %lu a %04x x %04x y %04x s %04x d %04x db %02x p %02x master %llu\n",
                         (unsigned long)frame, cpu_A(), cpu_X(), cpu_Y(), cpu_S(), cpu_D(), cpu_DB(),
                         cpu_P(), (unsigned long long)g_master_cycles);
      }
#endif
      if (frame % N64_STATE_HASH_PERIOD == 0) {
        uint32_t h = 2166136261u, ha = 2166136261u;
        for (int i = 0; i < 0x20000; ++i) h = (h ^ g_snes->ram[i]) * 16777619u;
        for (int i = 0; i < 0x10000; ++i) ha = (ha ^ g_snes->apu->ram[i]) * 16777619u;
        n64_platform_log("[state] frame %lu wram %08lx aram %08lx dsp-out %08lx (%lu samples)\n",
                         (unsigned long)frame, (unsigned long)h, (unsigned long)ha,
                         (unsigned long)s_out_hash, (unsigned long)s_out_pos);
        n64_platform_log("[spctime] frame %lu cycles %lu samples %lu pc %04x\n", (unsigned long)frame,
                         (unsigned long)g_snes->apu->cycles, (unsigned long)s_out_pos,
                         g_snes->apu->spc->pc);
#ifdef N64_RSPDSP
        { extern uint32_t g_snesdsp_rsp_ne_batches;
          n64_platform_log("[rspdsp] no-echo variant: %lu batches\n", (unsigned long)g_snesdsp_rsp_ne_batches); }
        { extern uint32_t g_snesdsp_rsp_samples, g_snesdsp_rsp_async, g_snesdsp_rsp_joins,
                          g_snesdsp_rsp_join_wait;
          n64_platform_log("[rspdsp] %lu of %lu samples ran on the RSP; %lu launches, %lu joins, "
                           "%lu us waiting\n", (unsigned long)g_snesdsp_rsp_samples,
                           (unsigned long)s_out_pos, (unsigned long)g_snesdsp_rsp_async,
                           (unsigned long)g_snesdsp_rsp_joins,
                           (unsigned long)(g_snesdsp_rsp_join_wait / 47));
          extern uint32_t g_apu_dsp_join_why[8];
          n64_platform_log("[rspdsp] joins by reason: host %lu regread %lu echoread %lu echoreg %lu "
                           "logfull %lu batch %lu frame/immediate %lu echowrite %lu\n",
                           (unsigned long)g_apu_dsp_join_why[0], (unsigned long)g_apu_dsp_join_why[1],
                           (unsigned long)g_apu_dsp_join_why[2], (unsigned long)g_apu_dsp_join_why[3],
                           (unsigned long)g_apu_dsp_join_why[4], (unsigned long)g_apu_dsp_join_why[5],
                           (unsigned long)g_apu_dsp_join_why[6], (unsigned long)g_apu_dsp_join_why[7]); }
#endif
      }
    }
#endif
#ifdef N64_MEM_REPORT
    if (frame == 299) {   /* RAM in use, once everything lazy is allocated */
      extern char end[];  /* end of the program's data and bss (an array: not gp-relative) */
      const struct mallinfo mi = mallinfo();
      n64_platform_log("[mem] RDRAM %lu KB; program image %lu KB; heap: break %lu KB above the image, "
                       "in use %lu KB, free inside %lu KB; above the break %lu KB\n",
                       (unsigned long)((*(volatile uint32_t *)0x80000318) / 1024),
                       (unsigned long)(((uintptr_t)end - 0x80000000u) / 1024),
                       (unsigned long)(((uintptr_t)sbrk(0) - (uintptr_t)end) / 1024),
                       (unsigned long)(mi.uordblks / 1024), (unsigned long)(mi.fordblks / 1024),
                       (unsigned long)((0x80000000u + (*(volatile uint32_t *)0x80000318) - (uintptr_t)sbrk(0)) / 1024));
    }
#endif
#if SNESRECOMP_APU_DEFER_DSP
    if (frame % 60 == 59) {
      static const char *const why[APU_DSP_SYNC_REASONS] = {"host", "regread", "echoread", "echoreg", "logfull", "batch", "frame"};
      char b[200]; int o = 0;
      for (int r = 0; r < APU_DSP_SYNC_REASONS; ++r) {
        o += snprintf(b + o, sizeof b - o, " %s %lu/%lu", why[r], (unsigned long)g_apu_dsp_sync_count[r],
                      (unsigned long)g_apu_dsp_sync_samples[r]);
        g_apu_dsp_sync_count[r] = g_apu_dsp_sync_samples[r] = 0;
      }
      n64_platform_log("[dspsync] 60 frames, syncs/samples:%s\n", b);
    }
#endif
#ifdef N64_INTERP_REPORT
    if (frame % 300 == 299) { extern void interp_report_dump(void); interp_report_dump(); }
#endif
#if SNESRECOMP_SPC_DBT
    if (frame % 60 == 59) {
      extern uint32_t g_spc_dbt_stats[8], g_spc_dbt_total;
      n64_platform_log("[spcdbt] 60 frames: SPC cycles %lu; blocks made %lu (%lu KB), flushes %lu, runs %lu, cycles %lu; "
                       "refused %lu, no room %lu, untranslatable %lu\n",
                       (unsigned long)g_spc_dbt_total,
                       (unsigned long)g_spc_dbt_stats[0], (unsigned long)(g_spc_dbt_stats[7] >> 10),
                       (unsigned long)g_spc_dbt_stats[1], (unsigned long)g_spc_dbt_stats[2],
                       (unsigned long)g_spc_dbt_stats[3], (unsigned long)g_spc_dbt_stats[4],
                       (unsigned long)g_spc_dbt_stats[5], (unsigned long)g_spc_dbt_stats[6]);
      { static uint32_t s_epoch0;
        n64_platform_log("[spcdbt]   epochs %lu\n", (unsigned long)(g_spc_dbt_epoch - s_epoch0));
        s_epoch0 = g_spc_dbt_epoch; }
      { extern uint32_t g_spc_dbt_refused[4], g_spc_dbt_code_flushes;
        n64_platform_log("[spcdbt]   refused: direct page %lu, watched read %lu, watched write %lu, code %lu; "
                         "flushes by code stores %lu\n",
                         (unsigned long)g_spc_dbt_refused[0], (unsigned long)g_spc_dbt_refused[1],
                         (unsigned long)g_spc_dbt_refused[2], (unsigned long)g_spc_dbt_refused[3],
                         (unsigned long)g_spc_dbt_code_flushes);
        memset(g_spc_dbt_refused, 0, sizeof g_spc_dbt_refused);
        extern uint32_t g_spc_dbt_chainend[8];
        n64_platform_log("[spcdbt]   links (slow path): patched %lu, no block %lu, not valid %lu, no time %lu, "
                         "wait loop %lu, went on %lu\n",
                         (unsigned long)g_spc_dbt_chainend[0], (unsigned long)g_spc_dbt_chainend[1],
                         (unsigned long)g_spc_dbt_chainend[2], (unsigned long)g_spc_dbt_chainend[3],
                         (unsigned long)g_spc_dbt_chainend[4], (unsigned long)g_spc_dbt_chainend[5]);
        memset(g_spc_dbt_chainend, 0, sizeof g_spc_dbt_chainend);
        g_spc_dbt_code_flushes = 0; }
      memset(g_spc_dbt_stats, 0, sizeof g_spc_dbt_stats);
      g_spc_dbt_total = 0;
    }
#endif
#ifdef SNESRECOMP_SPC_PC_PROFILE
    if (frame % 300 == 299) {
      extern uint32_t g_spc_pc_cycles[0x10000], g_spc_skip_cycles, g_spc_skip_count, g_spc_run_cycles;
      n64_platform_log("[spcprof] 300 frames: %lu cycles executed, %lu skipped in %lu skips\n",
                       (unsigned long)g_spc_run_cycles, (unsigned long)g_spc_skip_cycles,
                       (unsigned long)g_spc_skip_count);
      const uint8_t *ram = g_snes->apu->ram;
      { /* Executed cycles by opcode (the opcode now at each address), with
         * the running share: which opcodes a translator must cover. */
        static uint64_t by_op[256];
        uint64_t total = 0;
        memset(by_op, 0, sizeof by_op);
        for (int pc = 0; pc < 0x10000; pc++) { by_op[ram[pc]] += g_spc_pc_cycles[pc]; total += g_spc_pc_cycles[pc]; }
        uint64_t run = 0;
        char line[200]; int o = 0;
        for (int k = 0; k < 256 && total; k++) {
          int best = 0;
          for (int op = 1; op < 256; op++) if (by_op[op] > by_op[best]) best = op;
          if (!by_op[best]) break;
          run += by_op[best];
          o += snprintf(line + o, sizeof line - o, " %02x:%lu", best, (unsigned long)(run * 100 / total));
          by_op[best] = 0;
          if (o > 150) { n64_platform_log("[spcops]%s\n", line); o = 0; }
        }
        if (o) n64_platform_log("[spcops]%s\n", line);
      }
#ifdef N64_SPC_IDX_REPORT
      { static int dumped;
        if (!dumped++) {
          n64_platform_log("[spcidx-code]");
          for (int a = 0x1490; a < 0x1500; a++) n64_platform_log(" %02x", ram[a]);
          n64_platform_log("\n");
        } }
      { /* [dp+X] / [dp]+Y instructions executed, with their bytes. */
        for (int pc = 0; pc < 0x10000; pc++) {
          const uint8_t o = ram[pc];
          if (g_spc_pc_cycles[pc] && ((o & 0x0f) == 0x07 && (o & 0x10) == 0x00 || (o & 0x1f) == 0x17))
            n64_platform_log("[spcidx] %04x %02x %02x  %lu cycles\n", pc, o, ram[(uint16_t)(pc + 1)],
                             (unsigned long)g_spc_pc_cycles[pc]);
        }
      }
#endif
      for (int k = 0; k < 24; k++) {
        uint32_t best = 0; int bpc = -1;
        for (int pc = 0; pc < 0x10000; pc++) if (g_spc_pc_cycles[pc] > best) { best = g_spc_pc_cycles[pc]; bpc = pc; }
        if (bpc < 0) break;
        n64_platform_log("[spcprof]   %04x %02x %02x %02x %02x  %lu cycles (%lu.%lu%%)\n", bpc,
                         ram[bpc], ram[(uint16_t)(bpc + 1)], ram[(uint16_t)(bpc + 2)], ram[(uint16_t)(bpc + 3)],
                         (unsigned long)best, (unsigned long)(best * 100ull / (g_spc_run_cycles ? g_spc_run_cycles : 1)),
                         (unsigned long)(best * 1000ull / (g_spc_run_cycles ? g_spc_run_cycles : 1) % 10));
        g_spc_pc_cycles[bpc] = 0;
      }
      memset(g_spc_pc_cycles, 0, sizeof g_spc_pc_cycles);
      g_spc_skip_cycles = g_spc_skip_count = g_spc_run_cycles = 0;
    }
#endif
#ifdef N64_PHASE0
    if (frame % 60 == 59) {
      extern void n64_phase0_take(uint32_t *, uint32_t *, uint32_t *);
      extern uint32_t g_bgrsp_cmds, g_apu_port_writes, g_apu_catchups, g_apu_port_burst_max;
      uint32_t idle, words, wait;
      n64_phase0_take(&idle, &words, &wait);
      snprintf(s_p0_line, sizeof s_p0_line, "q%lu w%lu i%lu p%lu/%lu",
               (unsigned long)(words / 60), (unsigned long)(wait / 60 / 47),
               (unsigned long)idle,
               (unsigned long)(g_apu_port_writes / 60), (unsigned long)g_apu_port_burst_max);
      n64_platform_log("[phase0] per frame: rspq %lu words, BG %lu cmds, CPU waited %lu us for a queue buffer; "
                       "RSP idle %lu%%; APU port writes %lu, SPC runs %lu, max writes between runs %lu (60 frames)\n",
                       (unsigned long)(words / 60), (unsigned long)(g_bgrsp_cmds / 60),
                       (unsigned long)(wait / 60 / 47), (unsigned long)idle,
                       (unsigned long)(g_apu_port_writes / 60), (unsigned long)(g_apu_catchups / 60),
                       (unsigned long)g_apu_port_burst_max);
      {
        extern uint32_t g_apu_port_reads, g_apu_port_read_same, g_apu_outport_changes, g_apu_port_read_gap[5];
        extern uint64_t g_apu_port_read_cycles;
        n64_platform_log("[phase0] SPC per frame: CPU port reads %lu (same value %lu), SPC cycles per read %lu, "
                         "runs <64:%lu <256:%lu <1k:%lu <4k:%lu more:%lu, SPC port changes %lu, CPU port writes %lu\n",
                         (unsigned long)(g_apu_port_reads / 60), (unsigned long)(g_apu_port_read_same / 60),
                         (unsigned long)(g_apu_port_reads ? g_apu_port_read_cycles / g_apu_port_reads : 0),
                         (unsigned long)(g_apu_port_read_gap[0] / 60), (unsigned long)(g_apu_port_read_gap[1] / 60),
                         (unsigned long)(g_apu_port_read_gap[2] / 60), (unsigned long)(g_apu_port_read_gap[3] / 60),
                         (unsigned long)(g_apu_port_read_gap[4] / 60), (unsigned long)(g_apu_outport_changes / 60),
                         (unsigned long)(g_apu_port_writes / 60));
        g_apu_port_reads = g_apu_port_read_same = g_apu_outport_changes = 0;
        g_apu_port_read_cycles = 0;
        for (int i = 0; i < 5; i++) g_apu_port_read_gap[i] = 0;
      }
      g_bgrsp_cmds = g_apu_port_writes = g_apu_catchups = g_apu_port_burst_max = 0;
    }
#endif
#if SNESRECOMP_BLUE_THREAD
    if (frame % 60 == 59) {
      n64_platform_log("[blue] %lu pauses, %lu resumed, %lu unwound, %lu yields\n",
                       (unsigned long)g_blue_pauses, (unsigned long)g_blue_resumes,
                       (unsigned long)g_blue_unwinds, (unsigned long)g_blue_yields);
    }
#endif
#if defined(N64_RSPDSP) && !defined(N64_STATE_HASH)
    if (frame % 60 == 59) {
      extern uint32_t g_snesdsp_rsp_launch_ticks, g_snesdsp_rsp_join_ticks, g_snesdsp_rsp_join_wait,
                      g_snesdsp_rsp_pages, g_snesdsp_rsp_async, g_snesdsp_rsp_join_wait_blue,
                      g_snesdsp_rsp_decode_ticks, g_snesdsp_rsp_log_entries,
                      g_snesdsp_rsp_view_ticks, g_snesdsp_rsp_pack_ticks, g_snesdsp_rsp_submit_ticks;
      extern uint32_t g_snesdsp_rsp_reject[5];
      n64_platform_log("[rspdsp] batches left in C (60 frames): echo wraps %lu, pitch modulation %lu, other state %lu, log %lu, "
                       "no records %lu\n",
                       (unsigned long)g_snesdsp_rsp_reject[0], (unsigned long)g_snesdsp_rsp_reject[1],
                       (unsigned long)g_snesdsp_rsp_reject[2], (unsigned long)g_snesdsp_rsp_reject[3],
                       (unsigned long)g_snesdsp_rsp_reject[4]);
      memset(g_snesdsp_rsp_reject, 0, sizeof g_snesdsp_rsp_reject);
      extern uint32_t g_snesdsp_rsp_blue_yields, g_snesdsp_rsp_fallbacks, g_red_messages;
      n64_platform_log("[red] 60 frames: %lu messages; S-DSP waits: %lu Blue yields, %lu fallbacks\n",
                       (unsigned long)g_red_messages, (unsigned long)g_snesdsp_rsp_blue_yields,
                       (unsigned long)g_snesdsp_rsp_fallbacks);
      g_red_messages = g_snesdsp_rsp_blue_yields = g_snesdsp_rsp_fallbacks = 0;
      n64_platform_log("[rspdsp] launch parts per frame: view %lu us, pack %lu us, submit %lu us\n",
                       (unsigned long)(g_snesdsp_rsp_view_ticks / 60 / 47),
                       (unsigned long)(g_snesdsp_rsp_pack_ticks / 60 / 47),
                       (unsigned long)(g_snesdsp_rsp_submit_ticks / 60 / 47));
      g_snesdsp_rsp_view_ticks = g_snesdsp_rsp_pack_ticks = g_snesdsp_rsp_submit_ticks = 0;
      n64_platform_log("[rspdsp] per frame: launch %lu us (decode %lu us, %lu log entries), join %lu us (waiting %lu us, %lu on Blue), %lu launches, %lu pages\n",
                       (unsigned long)(g_snesdsp_rsp_launch_ticks / 60 / 47),
                       (unsigned long)(g_snesdsp_rsp_decode_ticks / 60 / 47),
                       (unsigned long)(g_snesdsp_rsp_log_entries / 60),
                       (unsigned long)(g_snesdsp_rsp_join_ticks / 60 / 47),
                       (unsigned long)(g_snesdsp_rsp_join_wait / 60 / 47),
                       (unsigned long)(g_snesdsp_rsp_join_wait_blue / 60 / 47),
                       (unsigned long)(g_snesdsp_rsp_async / 60), (unsigned long)(g_snesdsp_rsp_pages / 60));
      g_snesdsp_rsp_launch_ticks = g_snesdsp_rsp_join_ticks = g_snesdsp_rsp_join_wait = 0;
      g_snesdsp_rsp_join_wait_blue = g_snesdsp_rsp_decode_ticks = g_snesdsp_rsp_log_entries = 0;
      g_snesdsp_rsp_pages = g_snesdsp_rsp_async = 0;
    }
#endif
    if (++frame % 60 == 0) {
      n64_platform_log("[n64] frames %lu: avg/frame run %lu us, ppu %lu us, present %lu us\n",
                       (unsigned long)frame, (unsigned long)(acc_run / 60),
                       (unsigned long)(acc_draw / 60), (unsigned long)(acc_present / 60));
      acc_run = acc_draw = acc_present = 0;
#if N64_CODE_CACHE
      if (frame % 600 == 0) {
        RecompCacheStats st;
        recomp_cache_stats(&st);
        n64_platform_log("[n64] code cache: loads %u (%lu KB, %lu us avg, %lu us max on the CPU), "
                         "interpreted while loading %u, evictions %u, resident %u (%lu KB), over budget %u\n",
                         st.loads, (unsigned long)(st.load_bytes / 1024),
                         (unsigned long)(st.loads ? (s_cc_load_us + S_CC_BEGIN_US) / st.loads : 0),
                         (unsigned long)s_cc_load_max_us, st.fallbacks, st.evictions, st.resident,
                         (unsigned long)(st.used_bytes / 1024), st.over_budget);
      }
#endif
#if N64_ROM_PAGING
      if (s_rom_window && frame % 600 == 0)
        n64_platform_log("[n64] ROM pages copied in: %lu\n",
                         (unsigned long)n64_platform_rom_window_misses());
#endif
    }
  }
}
