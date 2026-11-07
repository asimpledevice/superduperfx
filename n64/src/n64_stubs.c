/* Symbols the interpreter-only N64 build provides in place of absent parts.
 *
 * Program tables: an interpreter-only build links no generated code, so the
 * dispatch and RAM-routine guard tables are empty and every guest call runs
 * in the interpreter tier. A generated tree (dispatch_v2.c) replaces these.
 *
 * MSU-1: msu1.c needs <dirent.h>, which libdragon does not provide. These
 * keep the chip permanently disabled, matching msu1.h's documented behaviour
 * when no MSU-1 data is available: reads return 0, writes are ignored. */
#include <stdbool.h>
#include <stdint.h>

#include "cpu_state.h"
#include "snes/msu1.h"

#ifndef SNESRECOMP_N64_HAS_GENERATED_CODE
const DispatchEntry g_dispatch_table[1];
const unsigned g_dispatch_table_count = 0;
const RamRoutineGuard g_ram_routine_guards[1];
const unsigned g_ram_routine_guard_count = 0;
#endif

void msu1_init(void) {}
bool msu1_enabled(void) { return false; }
uint8_t msu1_read(uint16_t reg) { (void)reg; return 0; }
void msu1_write(uint16_t reg, uint8_t val) { (void)reg; (void)val; }
