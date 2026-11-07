/* Fast read map. See cpu_fast_map.h for the contract. */
#include "cpu_fast_map.h"

#include <string.h>

#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/snes.h"

extern Snes *g_snes;

const uint8_t *g_cpu_fast_read_page[kCpuFastMapPages];
uint8_t g_cpu_fast_page_kind[kCpuFastMapPages];
static const uint8_t *s_built_for_rom;
static uint32_t s_built_for_rom_size;

static int is_system_bank(uint8_t bank) {
  return bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
}

/* Would cpu_read8 take its plain RomPtr path for every byte of this page,
 * and is that ROM contiguous across the page? Mirrors cpu_read8's order of
 * checks for LoROM/HiROM carts: WRAM, MMIO, (no coprocessor windows on these
 * cart types), SRAM, LoROM's $6000-$7FFF open bus, then ROM. */
static const uint8_t *rom_page_base(Cart *cart, uint8_t bank, uint16_t start) {
  if (bank == 0x7E || bank == 0x7F) return NULL;
  if (is_system_bank(bank) && start < 0x8000) return NULL; /* WRAM mirror, MMIO, SRAM/open bus */
  if (cart->ramSize > 0) {
    /* LoROM SRAM ($70-$7D/$F0-$FD:$0000-$7FFF). HiROM SRAM sits in system
     * banks below $8000, already excluded above. */
    if (cart->type == CART_LOROM && start < 0x8000 &&
        ((bank >= 0x70 && bank < 0x7E) || bank >= 0xF0))
      return NULL;
  }
  if (cart_is_cx4_window(cart, bank, start) || cart_is_dsp1_window(cart, bank, start) ||
      cart_is_dsp1_sram_window(cart, bank, start))
    return NULL;
  const uint8_t *p0 = cart_getRomPtr(cart, bank, start);
  if (!p0) return NULL;
  /* Contiguity at 256-byte steps plus the final byte. */
  for (uint32_t off = 0x100; off < 0x2000; off += 0x100)
    if (cart_getRomPtr(cart, bank, (uint16_t)(start + off)) != p0 + off) return NULL;
  if (cart_getRomPtr(cart, bank, (uint16_t)(start + 0x1FFF)) != p0 + 0x1FFF) return NULL;
  return p0;
}

void cpu_fast_map_rebuild(void) {
  memset(g_cpu_fast_read_page, 0, sizeof(g_cpu_fast_read_page));
  memset(g_cpu_fast_page_kind, 0, sizeof(g_cpu_fast_page_kind));
  s_built_for_rom = NULL;
  s_built_for_rom_size = 0;
#if SNESRECOMP_FAST_READ_MAP
  Cart *cart = g_snes ? g_snes->cart : NULL;
  if (!cart || !cart->rom) return;
  s_built_for_rom = cart->rom;
  s_built_for_rom_size = cart->romSize;
  /* Only plain cartridges: enhancement chips own parts of the map and some
   * change it at run time. */
  if (cart->type != CART_LOROM && cart->type != CART_HIROM) return;
  for (uint32_t page = 0; page < kCpuFastMapPages; ++page) {
    const uint8_t bank = (uint8_t)(page >> 3);
    const uint16_t start = (uint16_t)((page & 7u) << kCpuFastMapPageShift);
    if (bank == 0x7E || bank == 0x7F) {
      g_cpu_fast_read_page[page] = g_ram + ((bank & 1u) << 16) + start;
      g_cpu_fast_page_kind[page] = kCpuFastMapWram;
    } else if (is_system_bank(bank) && start == 0) {
      g_cpu_fast_read_page[page] = g_ram;
      g_cpu_fast_page_kind[page] = kCpuFastMapWramLow;
    } else {
      const uint8_t *p = rom_page_base(cart, bank, start);
      if (p) {
        g_cpu_fast_read_page[page] = p;
        g_cpu_fast_page_kind[page] = kCpuFastMapRom;
      }
    }
  }
#endif
}

void cpu_fast_map_validate(void) {
#if SNESRECOMP_FAST_READ_MAP
  const Cart *cart = g_snes ? g_snes->cart : NULL;
  const uint8_t *rom = cart ? cart->rom : NULL;
  const uint32_t size = cart ? cart->romSize : 0;
  if (rom != s_built_for_rom || size != s_built_for_rom_size) cpu_fast_map_rebuild();
#endif
}
