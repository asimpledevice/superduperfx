/* Fast read map: direct pointers for guest memory that a read cannot affect.
 *
 * One entry per 8 KiB page of the 24-bit CPU bus (2048 entries). An entry is
 * either a host pointer to the page's first byte or NULL. Only pages whose
 * every byte is plain WRAM or plain cartridge ROM get a pointer; everything
 * else (MMIO, SRAM, open-bus windows, coprocessor carts, enhancement-chip
 * mappers) stays NULL and keeps the full bus path.
 *
 * A non-NULL entry is a promise that cpu_read8() of any address in that page
 * would return exactly page[addr & 0x1FFF], apart from its two side effects,
 * which fast-path callers must reproduce: the open-bus latch and
 * cart_note_cpu_bus().
 *
 * Enabled with SNESRECOMP_FAST_READ_MAP=1 at build time. With it off,
 * cpu_fast_map_page() always returns NULL and callers fall back.
 *
 * The map is rebuilt by cpu_fast_map_rebuild() after a cartridge load, and
 * cpu_fast_map_validate() rebuilds it if the cartridge ROM buffer changed.
 * WRAM pages point into g_ram, which never moves. */
#ifndef SNESRECOMP_CPU_FAST_MAP_H
#define SNESRECOMP_CPU_FAST_MAP_H

#include <stdint.h>

#ifndef SNESRECOMP_FAST_READ_MAP
#define SNESRECOMP_FAST_READ_MAP 0
#endif

enum {
  kCpuFastMapPageShift = 13,
  kCpuFastMapPages = 1u << (24 - 13),
};

/* Page kinds, for callers that need to tell WRAM from ROM. */
enum {
  kCpuFastMapNone = 0,
  kCpuFastMapRom = 1,
  kCpuFastMapWram = 2,      /* banks $7E/$7F */
  kCpuFastMapWramLow = 3,   /* the $0000-$1FFF mirror in system banks */
};

extern const uint8_t *g_cpu_fast_read_page[kCpuFastMapPages];
extern uint8_t g_cpu_fast_page_kind[kCpuFastMapPages];

void cpu_fast_map_rebuild(void);
/* Cheap check (one pointer compare); rebuilds when the ROM buffer moved. */
void cpu_fast_map_validate(void);

static inline const uint8_t *cpu_fast_map_page(uint32_t adr24) {
#if SNESRECOMP_FAST_READ_MAP
  return g_cpu_fast_read_page[(adr24 & 0xFFFFFFu) >> kCpuFastMapPageShift];
#else
  (void)adr24;
  return 0;
#endif
}

#endif
