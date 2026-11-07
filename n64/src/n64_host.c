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

/* ---- ROM ------------------------------------------------------------- */

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

int main(void) {
  n64_platform_init();
  if (!n64_platform_has_expansion_pak())
    Die("This build needs the Expansion Pak (8 MB).");

  int rom_size = 0;
  uint8_t *rom = load_rom("rom:/game.sfc", &rom_size);
  if (!rom) Die("Could not read rom:/game.sfc. Rebuild with ROM=path/to/game.sfc.");
  n64_platform_log("[n64] ROM loaded: %d bytes\n", rom_size);

  s_pixels = calloc(kSnesW * kSnesH, sizeof(uint32_t));
  if (!s_pixels) Die("Out of memory for the PPU frame buffer.");

  RtlRegisterGame(&kN64GameInfo);
  if (!SnesInit(rom, rom_size)) Die("SnesInit failed: unsupported or damaged ROM.");
  free(rom);  /* the cartridge keeps its own copy (cart_load) */
  n64_platform_log("[n64] SNES initialized, entering main loop\n");

  uint32_t frame = 0;
  uint64_t acc_run = 0, acc_draw = 0, acc_present = 0;
  for (;;) {
    uint32_t inputs = n64_platform_read_pad();

    uint64_t t0 = n64_platform_time_us();
    RtlRunFrame(inputs);
    uint64_t t1 = n64_platform_time_us();
    PpuBeginDrawing(g_ppu, (uint8_t *)s_pixels, kSnesW * 4, kPpuRenderFlags_NewRenderer);
    n64_draw_ppu_frame();
    uint64_t t2 = n64_platform_time_us();

    char line[64];
    snprintf(line, sizeof line, "f%lu cpu %lums ppu %lums",
             (unsigned long)frame, (unsigned long)((t1 - t0) / 1000),
             (unsigned long)((t2 - t1) / 1000));
    n64_platform_present(s_pixels, kSnesW, kSnesH, line);
    uint64_t t3 = n64_platform_time_us();

    acc_run += t1 - t0;
    acc_draw += t2 - t1;
    acc_present += t3 - t2;
    if (++frame % 60 == 0) {
      n64_platform_log("[n64] frames %lu: avg/frame run %lu us, ppu %lu us, present %lu us\n",
                       (unsigned long)frame, (unsigned long)(acc_run / 60),
                       (unsigned long)(acc_draw / 60), (unsigned long)(acc_present / 60));
      acc_run = acc_draw = acc_present = 0;
    }
  }
}
