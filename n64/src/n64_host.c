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

#include "n64_platform.h"
#include "guest_fiber.h"
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

/* The beam driver's raster replay (snes_beam_frame_driver_draw_ppu_frame),
 * with each line expanded for the RDP and the software PPU run only where
 * needed. */
static void draw_frame_rdp(bool software_every_line) {
  snes_set_hdma_beam_enabled(g_snes, false);
  ppu_rasterRenderBegin(g_ppu);
  dma_initHdma(g_snes->dma);
  dma_doHdma(g_snes->dma);
  n64_ppu_build_begin(s_rdp_frame, s_pixels);
  for (int line = 1; line <= 224; line++) {
    uint8_t hdmaen;
    ppu_rasterApplyLine(g_ppu, line - 1);
    if (ppu_rasterTakeHdmaen(&hdmaen))
      dma_startDma(g_snes->dma, hdmaen, true);
    const bool rdp_ok = n64_ppu_build_line(g_ppu, line);
    if (software_every_line || !rdp_ok) ppu_runLine(g_ppu, line);
    else ppu_runLineState(g_ppu, line);
    dma_doHdma(g_snes->dma);
  }
  n64_ppu_build_end();
  ppu_rasterRenderEnd(g_ppu);
}

#if N64_RENDER
/* Finish a built frame (the RSP's background rows) and show it. */
static void present_rdp_frame(uint32_t frame, const char *line) {
  n64_ppu_build_finish();
  const int diff = n64_ppu_draw_and_show(s_rdp_frame, line, N64_RENDER == 2);
  if (N64_RENDER == 2 && (frame < 8 || frame % 30 == 0 || diff > 0))
    if (frame < 600 || frame % 30 == 0)
      n64_platform_log("[rdp] frame %lu: rdp %d sw %d black %d lines, why %02lx, diff %d px\n",
                       (unsigned long)frame, s_rdp_frame->rdp_lines,
                       s_rdp_frame->software_lines, s_rdp_frame->black_lines,
                       (unsigned long)s_rdp_frame->fallback_reasons, diff);
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

/* ---- Host contract (see runner/src/desktop/host_main.h) -------------- */

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
#if SNESRECOMP_GUEST_FIBER
/* The guest runs on its own stack (guest_fiber.h); its frames are the hot
 * ones, so it gets the placement the main stack was tuned for. */
static uint8_t s_guest_stack[256 * 1024] __attribute__((aligned(8192)));
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
static void code_cache_size(uint32_t id, uint32_t bytes) {
  if (id < g_recomp_routine_count) g_recomp_routines[id]->size = bytes;
}
static void code_cache_init(void) {
  for (uint32_t i = 0; i < g_recomp_routine_count; i++) g_recomp_routines[i]->id = i;
  const int n = n64_platform_codepack_open("rom:/cc.pack", code_cache_size);
  if (n != (int)g_recomp_routine_count)
    Die("Code cache: rom:/cc.pack is missing or does not match this build.");
  recomp_cache_set_loader(code_cache_load, code_cache_unload);
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

static int host_main(void) {
#if N64_STACK_COLOR >= 0 && defined(__mips__) && SNESRECOMP_GUEST_FIBER
  guest_fiber_set_stack(s_guest_stack, s_guest_stack + sizeof s_guest_stack - 8192 +
                                           (N64_STACK_COLOR & 8191) - 64);
#endif
  n64_platform_init();
  if (!n64_platform_has_expansion_pak())
    Die("This build needs the Expansion Pak (8 MB).");

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
    g_n64_profile_phase = N64_PHASE_CPU;
#if N64_RENDER
    n64_ppu_build_release_vram();   /* the RSP has its VRAM snapshot */
#endif
    RtlRunFrame(inputs);
    uint64_t t1 = n64_platform_time_us();
    g_n64_profile_phase = N64_PHASE_PRESENT;
#if N64_PIPELINE
    if (pending) present_rdp_frame(pending_frame, pending_line);
#endif
    uint64_t t1p = n64_platform_time_us();
    g_n64_profile_phase = N64_PHASE_PPU;
    PpuBeginDrawing(g_ppu, (uint8_t *)s_pixels, kSnesW * 4, kPpuRenderFlags_NewRenderer);
#if N64_RENDER
    draw_frame_rdp(N64_RENDER == 2);
#else
    n64_draw_ppu_frame();
#endif
    uint64_t t2 = n64_platform_time_us();
    g_n64_profile_phase = N64_PHASE_PRESENT;

    char line[64];
    snprintf(line, sizeof line, "f%lu cpu %lums ppu %lums",
             (unsigned long)frame, (unsigned long)((t1 - t0) / 1000),
             (unsigned long)((t2 - t1p) / 1000));
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
    n64_platform_audio_pump(render_audio);
    uint64_t t3 = n64_platform_time_us() + (t1p - t1);   /* present: both parts */

    if (frame < 8 || frame % 50 == 0)
      n64_platform_log("[n64] frame %lu: run %lu us, ppu %lu us\n", (unsigned long)frame,
                       (unsigned long)(t1 - t0), (unsigned long)(t2 - t1p));
    acc_run += t1 - t0;
    acc_draw += t2 - t1p;
    acc_present += t3 - t2;
#ifdef N64_PROFILE_FRAMES
    if (frame == N64_PROFILE_START) n64_platform_profile_start(N64_PROFILE_US);
    if (frame == N64_PROFILE_START + N64_PROFILE_FRAMES) n64_platform_profile_stop_and_dump();
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
      apu_dspSync(g_snes->apu);  /* all samples so far, also with async batches */
      const Dsp *d = g_snes->apu->dsp;
      for (; s_out_pos != d->sampleWrite; ++s_out_pos) {
        const uint32_t w = s_out_pos & (DSP_SAMPLE_RING - 1);
        s_out_hash = (s_out_hash ^ (uint16_t)d->sampleBuffer[w * 2]) * 16777619u;
        s_out_hash = (s_out_hash ^ (uint16_t)d->sampleBuffer[w * 2 + 1]) * 16777619u;
      }
#ifndef N64_STATE_HASH_PERIOD
#define N64_STATE_HASH_PERIOD 60
#endif
      if (frame % N64_STATE_HASH_PERIOD == 0) {
        uint32_t h = 2166136261u, ha = 2166136261u;
        for (int i = 0; i < 0x20000; ++i) h = (h ^ g_snes->ram[i]) * 16777619u;
        for (int i = 0; i < 0x10000; ++i) ha = (ha ^ g_snes->apu->ram[i]) * 16777619u;
        n64_platform_log("[state] frame %lu wram %08lx aram %08lx dsp-out %08lx (%lu samples)\n",
                         (unsigned long)frame, (unsigned long)h, (unsigned long)ha,
                         (unsigned long)s_out_hash, (unsigned long)s_out_pos);
#ifdef N64_RSPDSP
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
#if SNESRECOMP_APU_DEFER_DSP
    if (frame % 600 == 599) {
      static const char *const why[APU_DSP_SYNC_REASONS] = {"host", "regread", "echoread", "echoreg", "logfull", "batch"};
      char b[200]; int o = 0;
      for (int r = 0; r < APU_DSP_SYNC_REASONS; ++r) {
        o += snprintf(b + o, sizeof b - o, " %s %lu/%lu", why[r], (unsigned long)g_apu_dsp_sync_count[r],
                      (unsigned long)g_apu_dsp_sync_samples[r]);
        g_apu_dsp_sync_count[r] = g_apu_dsp_sync_samples[r] = 0;
      }
      n64_platform_log("[dspsync] 600 frames, syncs/samples:%s\n", b);
    }
#endif
#if SNESRECOMP_GUEST_FIBER
    if (frame % 60 == 59) {
      n64_platform_log("[fiber] %lu pauses, %lu resumed, %lu unwound\n",
                       (unsigned long)g_guest_fiber_pauses, (unsigned long)g_guest_fiber_resumes,
                       (unsigned long)g_guest_fiber_unwinds);
    }
#endif
#if defined(N64_RSPDSP) && !defined(N64_STATE_HASH)
    if (frame % 60 == 59) {
      extern uint32_t g_snesdsp_rsp_launch_ticks, g_snesdsp_rsp_join_ticks, g_snesdsp_rsp_join_wait,
                      g_snesdsp_rsp_pages, g_snesdsp_rsp_async;
      n64_platform_log("[rspdsp] per frame: launch %lu us, join %lu us (waiting %lu us), %lu launches, %lu pages\n",
                       (unsigned long)(g_snesdsp_rsp_launch_ticks / 60 / 47),
                       (unsigned long)(g_snesdsp_rsp_join_ticks / 60 / 47),
                       (unsigned long)(g_snesdsp_rsp_join_wait / 60 / 47),
                       (unsigned long)(g_snesdsp_rsp_async / 60), (unsigned long)(g_snesdsp_rsp_pages / 60));
      g_snesdsp_rsp_launch_ticks = g_snesdsp_rsp_join_ticks = g_snesdsp_rsp_join_wait = 0;
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
        n64_platform_log("[n64] code cache: loads %u (%lu KB, %lu us avg, %lu us max), evictions %u, "
                         "resident %u (%lu KB), over budget %u\n",
                         st.loads, (unsigned long)(st.load_bytes / 1024),
                         (unsigned long)(st.loads ? s_cc_load_us / st.loads : 0),
                         (unsigned long)s_cc_load_max_us, st.evictions, st.resident,
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
