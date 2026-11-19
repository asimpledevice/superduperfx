/*
 * interp816.c — 65816 interpreter core (the interpreter-fallback tier).
 *
 * Vendored from LakeSnes (https://github.com/angelo-wf/lakesnes), MIT,
 * Copyright (c) 2021-2023 angelo_wf and contributors.
 * See THIRD_PARTY_ATTRIBUTION.md.
 *
 * snesrecomp adaptation: Interp816 / interp816_ namespace; caller-supplied
 * callback memory bus (no direct snes_cpuRead/Write); debug instrumentation
 * (pc_hist / DumpCpuHistory / top-of-doOpcode assert) stripped; BRK can use
 * historical fallback-tier marker behavior or architectural vectoring. See
 * docs/MULTI_TIER.md.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include "interp816.h"

/* Register access. Every read and write of the 65816 registers and flags in
 * this file goes through I816_R / I816_W, so the same source builds two
 * cores: this one, on the Interp816 struct (coprocessors such as the SA-1),
 * and the main CPU's (interp816_main.c), whose registers are the reserved
 * FPU registers compiled code already uses (cpu_regs.h). pc, the interrupt
 * lines and the power state are always in the struct. */
#ifndef I816_R
#define I816_R(f) (cpu->f)
#define I816_W(f, v) (cpu->f = (v))
#endif

/* SNESRECOMP_COMPACT_INTERP=1 (the N64 build): the whole core is compiled for
 * size, so the interpreter's working set fits the VR4300's 16 KB I-cache
 * (docs/n64/BRINGUP_NOTES.md, "Interpreter kernel"). */
#ifndef SNESRECOMP_COMPACT_INTERP
#define SNESRECOMP_COMPACT_INTERP 0
#endif
#if SNESRECOMP_COMPACT_INTERP && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#if defined(__GNUC__)
#define I816_UNREACHABLE() __builtin_unreachable()
#else
#define I816_UNREACHABLE() ((void)0)
#endif

/* The tables and tallies below are defined once, in the struct build, and
 * shared by the main CPU's build (I816_MAIN). */
#ifdef I816_MAIN
extern const uint8_t cyclesPerOpcode[256];
#else
const uint8_t cyclesPerOpcode[256] = {   /* (bytes: as ints the table was 1 KB of the 8 KB data cache) */
  7, 6, 7, 4, 5, 3, 5, 6, 3, 2, 2, 4, 6, 4, 6, 5,
  2, 5, 5, 7, 5, 4, 6, 6, 2, 4, 2, 2, 6, 4, 7, 5,
  6, 6, 8, 4, 3, 3, 5, 6, 4, 2, 2, 5, 4, 4, 6, 5,
  2, 5, 5, 7, 4, 4, 6, 6, 2, 4, 2, 2, 4, 4, 7, 5,
  6, 6, 2, 4, 7, 3, 5, 6, 3, 2, 2, 3, 3, 4, 6, 5,
  2, 5, 5, 7, 7, 4, 6, 6, 2, 4, 3, 2, 4, 4, 7, 5,
  6, 6, 6, 4, 3, 3, 5, 6, 4, 2, 2, 6, 5, 4, 6, 5,
  2, 5, 5, 7, 4, 4, 6, 6, 2, 4, 4, 2, 6, 4, 7, 5,
  3, 6, 4, 4, 3, 3, 3, 6, 2, 2, 2, 3, 4, 4, 4, 5,
  2, 6, 5, 7, 4, 4, 4, 6, 2, 5, 2, 2, 4, 5, 5, 5,
  2, 6, 2, 4, 3, 3, 3, 6, 2, 2, 2, 4, 4, 4, 4, 5,
  2, 5, 5, 7, 4, 4, 4, 6, 2, 4, 2, 2, 4, 4, 4, 5,
  2, 6, 3, 4, 3, 3, 5, 6, 2, 2, 2, 3, 4, 4, 6, 5,
  2, 5, 5, 7, 6, 4, 6, 6, 2, 4, 3, 3, 6, 4, 7, 5,
  2, 6, 3, 4, 3, 3, 5, 6, 2, 2, 2, 3, 4, 4, 6, 5,
  2, 5, 5, 7, 5, 4, 6, 6, 2, 4, 4, 2, 8, 4, 7, 5
};
#endif

static uint8_t interp816_read(Interp816* cpu, uint32_t adr);
static void interp816_write(Interp816* cpu, uint32_t adr, uint8_t val);
static uint8_t interp816_readOpcode(Interp816* cpu);
static uint16_t interp816_readOpcodeWord(Interp816* cpu);
static void interp816_setZN(Interp816* cpu, uint16_t value, bool byte);
static void interp816_doBranch(Interp816* cpu, uint8_t value, bool check);
static uint8_t interp816_pullByte(Interp816* cpu);
static void interp816_pushByte(Interp816* cpu, uint8_t value);
static uint16_t interp816_pullWord(Interp816* cpu);
static void interp816_pushWord(Interp816* cpu, uint16_t value);
static uint16_t interp816_readWord(Interp816* cpu, uint32_t adrl, uint32_t adrh);
static void interp816_writeWord(Interp816* cpu, uint32_t adrl, uint32_t adrh, uint16_t value, bool reversed);
static void interp816_doInterrupt(Interp816* cpu, bool irq);
static void interp816_doOpcode(Interp816* cpu, uint8_t opcode);


// addressing modes and opcode functions not declared, only used after defintions

static uint8_t interp816_read(Interp816* cpu, uint32_t adr) {
  return cpu->read(cpu->mem, adr);
}

static void interp816_write(Interp816* cpu, uint32_t adr, uint8_t val) {
  cpu->write(cpu->mem, adr, val);
}

#ifndef I816_MAIN
Interp816* interp816_init(void* mem, Interp816ReadHandler read, Interp816WriteHandler write) {
  Interp816* cpu = malloc(sizeof(Interp816));
  memset(cpu, 0, sizeof(Interp816));
  cpu->mem = mem;
  cpu->read = read;
  cpu->write = write;
  cpu->brkHookEnabled = true;
  return cpu;
}

void interp816_free(Interp816* cpu) {
  free(cpu);
}
#endif

void interp816_reset(Interp816* cpu) {
  I816_W(a, 0);
  I816_W(x, 0);
  I816_W(y, 0);
  I816_W(sp, 0x100);
  cpu->pc = interp816_read(cpu, 0xfffc) | (interp816_read(cpu, 0xfffd) << 8);
  I816_W(dp, 0);
  I816_W(k, 0);
  I816_W(db, 0);
  I816_W(c, false);
  I816_W(z, false);
  I816_W(v, false);
  I816_W(n, false);
  I816_W(i, true);
  I816_W(d, false);
  I816_W(xf, true);
  I816_W(mf, true);
  I816_W(e, true);
  cpu->irqWanted = false;
  cpu->nmiWanted = false;
  cpu->waiting = false;
  cpu->stopped = false;
  cpu->cyclesUsed = 0;
}

#ifndef I816_MAIN
void interp816_set_brk_hook_enabled(Interp816 *cpu, bool enabled) {
  if (cpu) cpu->brkHookEnabled = enabled;
}

void interp816_saveload(Interp816 *cpu, SaveLoadInfo *sli) {
  sli->func(sli, &cpu->a, offsetof(Interp816, cyclesUsed) - offsetof(Interp816, a));
}

/* Current opcode PC (PB:PC of the opcode byte). Updated every interpreted
 * instruction so WRAM write-watches can name the store site under LLE. */
uint32_t g_interp816_cur_pc = 0;


/* Always-on interpreter execution tally: instructions and guest cycles
 * actually run by the 65816 interpreter tier. Divided by g_master_cycles
 * (total guest cycles, AOT+interp) this gives the mode-independent fraction
 * of execution that is NOT statically recompiled. Read via interp816_*_total. */
uint64_t snesrecomp_host_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t s_interp816_insns;
uint64_t s_interp816_cycles;
uint64_t s_interp816_opcodes_run = 0; /* dev perf: per-bridge-call opcode count */
uint64_t interp816_insns_total(void)  { return s_interp816_insns; }
uint64_t interp816_cycles_total(void) { return s_interp816_cycles; }

/* ── Always-on interpreted-execution map, by 256-byte guest page ───────────
 *
 * interp_stats answers "how much of the guest is interpreted"; it cannot
 * answer "WHICH code". Without that second answer a coverage burndown is
 * guesswork: a session reads 99% interpreted, picks the routine it happens to
 * be looking at, and finds out after a regen whether that was the dominant
 * class. This is the instrument that names the class BEFORE the work.
 *
 * Accumulated from instruction zero in every configuration, Release included
 * (PRINCIPLES.md, "Always-On Rings, Never Arm-and-Hope"): a probe queries the
 * window it cares about, it never arms a recorder and re-runs a workload.
 * Page granularity (pc24 >> 8) is 64K counters -- 1 MB of BSS, one indexed
 * 64-bit add per interpreted instruction, which is noise beside the
 * interpreter's own per-opcode cost -- and it is fine enough to name a
 * routine against the cfg symbol map while being immune to the aliasing a
 * hashed-PC sampler suffers from.
 *
 * Cycles, not instructions, is the headline: interp_stats reports the same
 * unit, so a page's share here compares directly with the whole-run total. Instructions are kept beside
 * it because a page that is all long-cycle opcodes and a page that is a tight
 * two-cycle loop want different fixes. */
/* SNESRECOMP_INTERP_PAGE_MAP=0 drops the map (1 MiB of counters) for
 * memory-constrained hosts; the accessors then report zero. */
#ifndef SNESRECOMP_INTERP_PAGE_MAP
#define SNESRECOMP_INTERP_PAGE_MAP 1
#endif
#if SNESRECOMP_INTERP_PAGE_MAP
#define INTERP816_PAGE_COUNT 0x10000u
#else
#define INTERP816_PAGE_COUNT 1u
#endif
uint64_t s_interp_page_cycles[INTERP816_PAGE_COUNT];
uint64_t s_interp_page_insns[INTERP816_PAGE_COUNT];

uint64_t interp816_page_cycles(unsigned page) {
    return page < INTERP816_PAGE_COUNT ? s_interp_page_cycles[page] : 0;
}
uint64_t interp816_page_insns(unsigned page) {
    return page < INTERP816_PAGE_COUNT ? s_interp_page_insns[page] : 0;
}
unsigned interp816_page_count(void) { return INTERP816_PAGE_COUNT; }

/* Dev hotspot sampler (SNESRECOMP_PHASE_MS): top EXACT PCs executed by the
 * interpreter, dumped via interp816_perf_dump(). Keyed by a 12-bit hash of
 * the full pc24 so the dump shows the precise hot instruction, not a page. */
#define INTERP_PC_BUCKETS 4096
uint32_t s_pc_buckets[INTERP_PC_BUCKETS];
uint32_t s_pc24[INTERP_PC_BUCKETS];
uint32_t s_pc_bucket_total = 0;
uint32_t s_pc_sample_skip = 0;
uint32_t s_oh_count[256], s_oh_tot[256]; /* per-opcode: samples, host-ns */
uint32_t s_oh_sample; /* 1-in-64 probe counter */
int s_oh_on = -1;
#define OPCODE_HIST_BEGIN() do { uint64_t _oh_t0 = 0; if (s_oh_on < 0) { const char *_e = getenv("SNESRECOMP_INTERP_OPCODE_HIST"); s_oh_on = (_e && _e[0] && _e[0] != '0') ? 1 : 0; } int _oh_s = (s_oh_on == 1 && (s_oh_sample++ & 63u) == 0); if (_oh_s) _oh_t0 = snesrecomp_host_now_ns();
#define OPCODE_HIST_END(_op) if (_oh_s) { uint64_t _dt = snesrecomp_host_now_ns() - _oh_t0; s_oh_count[(_op)]++; s_oh_tot[(_op)] += (uint32_t)(_dt < 0xFFFFFFFFu ? _dt : 0xFFFFFFFFu); } } while(0)
void interp816_perf_dump(void) {
    uint32_t idx[INTERP_PC_BUCKETS];
    for (int i = 0; i < INTERP_PC_BUCKETS; i++) idx[i] = i;
    for (int i = 1; i < INTERP_PC_BUCKETS; i++) {
        uint32_t v = idx[i]; int j = i - 1;
        while (j >= 0 && s_pc_buckets[idx[j]] < s_pc_buckets[v]) { idx[j+1] = idx[j]; j--; }
        idx[j+1] = v;
    }
    fprintf(stderr, "[phase] top interp PCs (sampled %u):\n", s_pc_bucket_total);
    for (int k = 0; k < 16 && k < INTERP_PC_BUCKETS; k++) {
        int i = idx[k];
        if (!s_pc_buckets[i]) break;
        fprintf(stderr, "  $%06X  %u (%.1f%%)\n", s_pc24[i],
                s_pc_buckets[i], 100.0 * s_pc_buckets[i] / s_pc_bucket_total);
    }
    memset(s_pc_buckets, 0, sizeof(s_pc_buckets));
    memset(s_pc24, 0, sizeof(s_pc24));
    s_pc_bucket_total = 0;
}

/* Dump the opcode-cost histogram (SNESRECOMP_INTERP_OPCODE_HIST) sorted by
 * total host-ns, with per-opcode sample count and avg ns/opcode. */
void interp816_opcode_hist_dump(void) {
    uint64_t sum = 0;
    int cnt = 0;
    int idx[256];
    for (int i = 0; i < 256; i++) {
        idx[i] = i;
        sum += s_oh_tot[i];
        cnt += s_oh_count[i];
    }
    for (int i = 1; i < 256; i++) {
        uint32_t v = idx[i];
        int j = i - 1;
        while (j >= 0 && s_oh_tot[idx[j]] < s_oh_tot[v]) { idx[j+1] = idx[j]; j--; }
        idx[j+1] = v;
    }
    fprintf(stderr, "[opcode_hist] samples=%d totalhost_ns=%llu\n",
            cnt, (unsigned long long)sum);
    for (int k = 0; k < 24 && k < 256; k++) {
        int i = idx[k];
        if (!s_oh_count[i]) break;
        fprintf(stderr, "  op=%02X cnt=%u tot_ns=%u avg=%llu (%.1f%%)\n",
                i, s_oh_count[i], s_oh_tot[i],
                (unsigned long long)(s_oh_tot[i] / (s_oh_count[i] ? s_oh_count[i] : 1)),
                100.0 * s_oh_tot[i] / (sum ? sum : 1));
    }
    memset(s_oh_count, 0, sizeof(s_oh_count));
    memset(s_oh_tot, 0, sizeof(s_oh_tot));
}

#else
extern uint32_t g_interp816_cur_pc;
extern uint64_t s_interp816_insns, s_interp816_cycles, s_interp816_opcodes_run;
extern uint64_t s_interp_page_cycles[], s_interp_page_insns[];
extern uint32_t s_pc_buckets[], s_pc24[], s_pc_bucket_total, s_pc_sample_skip;
extern uint32_t s_oh_count[256], s_oh_tot[256], s_oh_sample;
extern int s_oh_on;
uint64_t snesrecomp_host_now_ns(void);
#ifndef SNESRECOMP_INTERP_PAGE_MAP
#define SNESRECOMP_INTERP_PAGE_MAP 1
#endif
#define INTERP_PC_BUCKETS 4096
#define OPCODE_HIST_BEGIN() do { uint64_t _oh_t0 = 0; if (s_oh_on < 0) { const char *_e = getenv("SNESRECOMP_INTERP_OPCODE_HIST"); s_oh_on = (_e && _e[0] && _e[0] != '0') ? 1 : 0; } int _oh_s = (s_oh_on == 1 && (s_oh_sample++ & 63u) == 0); if (_oh_s) _oh_t0 = snesrecomp_host_now_ns();
#define OPCODE_HIST_END(_op) if (_oh_s) { uint64_t _dt = snesrecomp_host_now_ns() - _oh_t0; s_oh_count[(_op)]++; s_oh_tot[(_op)] += (uint32_t)(_dt < 0xFFFFFFFFu ? _dt : 0xFFFFFFFFu); } } while(0)
#endif

/* The dev hotspot sampler and the per-opcode host-time histogram each read
 * a static on every instruction to see whether an environment variable
 * turned them on. Nothing sets one on the N64, and that load was the hottest
 * instruction of the core in a PC-sample profile (a data-cache miss), so
 * they are compiled in only on request. */
#ifndef SNESRECOMP_INTERP_DEV_PROBES
#define SNESRECOMP_INTERP_DEV_PROBES 0
#endif
#if !SNESRECOMP_INTERP_DEV_PROBES
#undef OPCODE_HIST_BEGIN
#undef OPCODE_HIST_END
#define OPCODE_HIST_BEGIN() do {
#define OPCODE_HIST_END(_op) } while (0)
#endif
int interp816_runOpcode(Interp816* cpu) {
  cpu->cyclesUsed = 0;
  s_interp816_opcodes_run++;
#if SNESRECOMP_INTERP_DEV_PROBES
  /* getenv() walks the whole environment block on MSVC (~us) - cache it so
   * the hot path pays a branch, not a CRT call, per interpreted instruction.
   * Same value as getenv, so behaviour is bit-identical. */
  {
    static int s_phase_ms = -1;
    if (s_phase_ms < 0) s_phase_ms = getenv("SNESRECOMP_PHASE_MS") ? 1 : 0;
    if (s_phase_ms) {
      if (++s_pc_sample_skip >= 8) {
          s_pc_sample_skip = 0;
          uint32_t pc = ((uint32_t)I816_R(k) << 16) | cpu->pc;
          uint32_t h = (pc * 2654435761u) >> (32 - 12);
          s_pc_buckets[h]++;
          s_pc24[h] = pc;
          s_pc_bucket_total++;
      }
    }
  }
#endif
  if(cpu->stopped) return 1;

  bool interruptPending = cpu->nmiWanted || cpu->irqWanted;
  if(cpu->waiting) {
    if(!interruptPending) return 1;
    cpu->waiting = false;
  }

  if(cpu->nmiWanted) {
    cpu->nmiWanted = false;
    cpu->cyclesUsed = 7;
    interp816_doInterrupt(cpu, false);
    return cpu->cyclesUsed;
  }
  if(!I816_R(i) && cpu->irqWanted) {
    cpu->irqWanted = false;
    cpu->cyclesUsed = 7;
    interp816_doInterrupt(cpu, true);
    return cpu->cyclesUsed;
  }

  uint8_t opcode = interp816_readOpcode(cpu);
  uint32_t _pcb = ((uint32_t)I816_R(k) << 16) | (uint16_t)(cpu->pc - 1);
  g_interp816_cur_pc = _pcb;
  cpu->cyclesUsed = cyclesPerOpcode[opcode];
  /* Dev opcode-cost histogram (SNESRECOMP_INTERP_OPCODE_HIST). Measures host
   * ns spent INSIDE doOpcode per opcode, sampled every 64 instructions so the
   * probe itself doesn't skew the number under test. Dumped via
   * interp816_opcode_hist_dump(). No behaviour change. */
  OPCODE_HIST_BEGIN();
  interp816_doOpcode(cpu, opcode);
  OPCODE_HIST_END(opcode);
  s_interp816_insns++;
  s_interp816_cycles += (uint64_t)cpu->cyclesUsed;
#if SNESRECOMP_INTERP_PAGE_MAP
  {
    /* Charge the page the OPCODE stood on (_pcb), not the post-execute PC,
     * so a branch or a jump is attributed to the routine that issued it. */
    const unsigned _pg = (unsigned)((_pcb >> 8) & 0xFFFFu);
    s_interp_page_cycles[_pg] += (uint64_t)cpu->cyclesUsed;
    s_interp_page_insns[_pg]++;
  }
#endif
  return cpu->cyclesUsed;
}

static uint8_t interp816_readOpcode(Interp816* cpu) {
  return interp816_read(cpu, (I816_R(k) << 16) | cpu->pc++);
}

static uint16_t interp816_readOpcodeWord(Interp816* cpu) {
  uint8_t low = interp816_readOpcode(cpu);
  return low | (interp816_readOpcode(cpu) << 8);
}

uint8_t interp816_getFlags(Interp816* cpu) {
  uint8_t val = I816_R(n) << 7;
  val |= I816_R(v) << 6;
  val |= I816_R(mf) << 5;
  val |= I816_R(xf) << 4;
  val |= I816_R(d) << 3;
  val |= I816_R(i) << 2;
  val |= I816_R(z) << 1;
  val |= I816_R(c);
  return val;
}

void interp816_setFlags(Interp816* cpu, uint8_t val) {
  I816_W(n, val & 0x80);
  I816_W(v, val & 0x40);
  I816_W(mf, val & 0x20);
  I816_W(xf, val & 0x10);
  I816_W(d, val & 8);
  I816_W(i, val & 4);
  I816_W(z, val & 2);
  I816_W(c, val & 1);
  if(I816_R(e)) {
    I816_W(mf, true);
    I816_W(xf, true);
    I816_W(sp, (I816_R(sp) & 0xff) | 0x100);
  }
  if(I816_R(xf)) {
    I816_W(x, I816_R(x) & (0xff));
    I816_W(y, I816_R(y) & (0xff));
  }
}

static void interp816_setZN(Interp816* cpu, uint16_t value, bool byte) {
#ifdef I816_MAIN
  (void)cpu;
  if (byte) cpu_set_nz8(value); else cpu_set_nz16(value);
  return;
#endif
  if(byte) {
    I816_W(z, (value & 0xff) == 0);
    I816_W(n, value & 0x80);
  } else {
    I816_W(z, value == 0);
    I816_W(n, value & 0x8000);
  }
}

static void interp816_doBranch(Interp816* cpu, uint8_t value, bool check) {
  if(check) {
    cpu->cyclesUsed++; // taken branch: 1 extra cycle
    cpu->pc += (int8_t) value;
  }
}

static uint8_t interp816_pullByte(Interp816* cpu) {
  I816_W(sp, I816_R(sp) + 1);
  if(I816_R(e)) I816_W(sp, (I816_R(sp) & 0xff) | 0x100);
  return interp816_read(cpu, I816_R(sp));
}

static void interp816_pushByte(Interp816* cpu, uint8_t value) {
  interp816_write(cpu, I816_R(sp), value);
  I816_W(sp, I816_R(sp) - 1);
  if(I816_R(e)) I816_W(sp, (I816_R(sp) & 0xff) | 0x100);
}

static uint16_t interp816_pullWord(Interp816* cpu) {
  uint8_t value = interp816_pullByte(cpu);
  return value | (interp816_pullByte(cpu) << 8);
}

static void interp816_pushWord(Interp816* cpu, uint16_t value) {
  interp816_pushByte(cpu, value >> 8);
  interp816_pushByte(cpu, value & 0xff);
}

static uint16_t interp816_readWord(Interp816* cpu, uint32_t adrl, uint32_t adrh) {
  if(cpu->read_word) {
    uint16_t v;
    if(cpu->read_word(cpu->mem, adrl, adrh, &v)) return v;
  }
  uint8_t value = interp816_read(cpu, adrl);
  return value | (interp816_read(cpu, adrh) << 8);
}

static void interp816_writeWord(Interp816* cpu, uint32_t adrl, uint32_t adrh, uint16_t value, bool reversed) {
  if(cpu->write_word && cpu->write_word(cpu->mem, adrl, adrh, value, reversed))
    return;
  if(reversed) {
    interp816_write(cpu, adrh, value >> 8);
    interp816_write(cpu, adrl, value & 0xff);
  } else {
    interp816_write(cpu, adrl, value & 0xff);
    interp816_write(cpu, adrh, value >> 8);
  }
}

static void interp816_doInterrupt(Interp816* cpu, bool irq) {
  interp816_pushByte(cpu, I816_R(k));
  interp816_pushWord(cpu, cpu->pc);
  interp816_pushByte(cpu, interp816_getFlags(cpu));
  cpu->cyclesUsed++; // native mode: 1 extra cycle
  I816_W(i, true);
  I816_W(d, false);
  I816_W(k, 0);
  if(irq) {
    cpu->pc = interp816_readWord(cpu, 0xffee, 0xffef);
  } else {
    // nmi
    cpu->pc = interp816_readWord(cpu, 0xffea, 0xffeb);
  }
}

// addressing modes

static uint32_t interp816_adrImm(Interp816* cpu, uint32_t* low, bool xFlag) {
  if((xFlag && I816_R(xf)) || (!xFlag && I816_R(mf))) {
    *low = (I816_R(k) << 16) | cpu->pc++;
    return 0;
  } else {
    *low = (I816_R(k) << 16) | cpu->pc++;
    return (I816_R(k) << 16) | cpu->pc++;
  }
}

static uint32_t interp816_adrDp(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(I816_R(dp) & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  *low = (I816_R(dp) + adr) & 0xffff;
  return (I816_R(dp) + adr + 1) & 0xffff;
}

static uint32_t interp816_adrDpx(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(I816_R(dp) & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  *low = (I816_R(dp) + adr + I816_R(x)) & 0xffff;
  return (I816_R(dp) + adr + I816_R(x) + 1) & 0xffff;
}

static uint32_t interp816_adrDpy(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(I816_R(dp) & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  *low = (I816_R(dp) + adr + I816_R(y)) & 0xffff;
  return (I816_R(dp) + adr + I816_R(y) + 1) & 0xffff;
}

static uint32_t interp816_adrIdp(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(I816_R(dp) & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint16_t pointer = interp816_readWord(cpu, (I816_R(dp) + adr) & 0xffff, (I816_R(dp) + adr + 1) & 0xffff);
  *low = (I816_R(db) << 16) + pointer;
  return ((I816_R(db) << 16) + pointer + 1) & 0xffffff;
}

static uint32_t interp816_adrIdx(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(I816_R(dp) & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint16_t pointer = interp816_readWord(cpu, (I816_R(dp) + adr + I816_R(x)) & 0xffff, (I816_R(dp) + adr + I816_R(x) + 1) & 0xffff);
  *low = (I816_R(db) << 16) + pointer;
  return ((I816_R(db) << 16) + pointer + 1) & 0xffffff;
}

static uint32_t interp816_adrIdy(Interp816* cpu, uint32_t* low, bool write) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(I816_R(dp) & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint16_t pointer = interp816_readWord(cpu, (I816_R(dp) + adr) & 0xffff, (I816_R(dp) + adr + 1) & 0xffff);
  bool crossed = (pointer >> 8) != ((pointer + I816_R(y)) >> 8);
  // W65C816S cycle-table note 4: read penalty for X=0 OR crossing.
  // Store/RMW base cycles already include the unconditional indexing cycle.
  if(!write && (!I816_R(xf) || crossed)) cpu->cyclesUsed++;
  *low = ((I816_R(db) << 16) + pointer + I816_R(y)) & 0xffffff;
  return ((I816_R(db) << 16) + pointer + I816_R(y) + 1) & 0xffffff;
}

static uint32_t interp816_adrIdl(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(I816_R(dp) & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint32_t pointer = interp816_readWord(cpu, (I816_R(dp) + adr) & 0xffff, (I816_R(dp) + adr + 1) & 0xffff);
  pointer |= interp816_read(cpu, (I816_R(dp) + adr + 2) & 0xffff) << 16;
  *low = pointer;
  return (pointer + 1) & 0xffffff;
}

static uint32_t interp816_adrIly(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(I816_R(dp) & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint32_t pointer = interp816_readWord(cpu, (I816_R(dp) + adr) & 0xffff, (I816_R(dp) + adr + 1) & 0xffff);
  pointer |= interp816_read(cpu, (I816_R(dp) + adr + 2) & 0xffff) << 16;
  *low = (pointer + I816_R(y)) & 0xffffff;
  return (pointer + I816_R(y) + 1) & 0xffffff;
}

static uint32_t interp816_adrSr(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  *low = (I816_R(sp) + adr) & 0xffff;
  return (I816_R(sp) + adr + 1) & 0xffff;
}

static uint32_t interp816_adrIsy(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  uint16_t pointer = interp816_readWord(cpu, (I816_R(sp) + adr) & 0xffff, (I816_R(sp) + adr + 1) & 0xffff);
  *low = ((I816_R(db) << 16) + pointer + I816_R(y)) & 0xffffff;
  return ((I816_R(db) << 16) + pointer + I816_R(y) + 1) & 0xffffff;
}

static uint32_t interp816_adrAbs(Interp816* cpu, uint32_t* low) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  *low = (I816_R(db) << 16) + adr;
  return ((I816_R(db) << 16) + adr + 1) & 0xffffff;
}

static uint32_t interp816_adrAbx(Interp816* cpu, uint32_t* low, bool write) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  bool crossed = (adr >> 8) != ((adr + I816_R(x)) >> 8);
  if(!write && (!I816_R(xf) || crossed)) cpu->cyclesUsed++;
  *low = ((I816_R(db) << 16) + adr + I816_R(x)) & 0xffffff;
  return ((I816_R(db) << 16) + adr + I816_R(x) + 1) & 0xffffff;
}

static uint32_t interp816_adrAby(Interp816* cpu, uint32_t* low, bool write) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  bool crossed = (adr >> 8) != ((adr + I816_R(y)) >> 8);
  if(!write && (!I816_R(xf) || crossed)) cpu->cyclesUsed++;
  *low = ((I816_R(db) << 16) + adr + I816_R(y)) & 0xffffff;
  return ((I816_R(db) << 16) + adr + I816_R(y) + 1) & 0xffffff;
}

static uint32_t interp816_adrAbl(Interp816* cpu, uint32_t* low) {
  uint32_t adr = interp816_readOpcodeWord(cpu);
  adr |= interp816_readOpcode(cpu) << 16;
  *low = adr;
  return (adr + 1) & 0xffffff;
}

static uint32_t interp816_adrAlx(Interp816* cpu, uint32_t* low) {
  uint32_t adr = interp816_readOpcodeWord(cpu);
  adr |= interp816_readOpcode(cpu) << 16;
  *low = (adr + I816_R(x)) & 0xffffff;
  return (adr + I816_R(x) + 1) & 0xffffff;
}

static uint16_t interp816_adrIax(Interp816* cpu) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  return interp816_readWord(cpu, (I816_R(k) << 16) | ((adr + I816_R(x)) & 0xffff), (I816_R(k) << 16) | ((adr + I816_R(x) + 1) & 0xffff));
}

// opcode functions




static void interp816_adc(Interp816* cpu, uint32_t low, uint32_t high) {
  if(I816_R(mf)) {
    uint8_t value = interp816_read(cpu, low);
    int result = 0;
    if(I816_R(d)) {
      result = (I816_R(a) & 0xf) + (value & 0xf) + I816_R(c);
      if(result > 0x9) result = ((result + 0x6) & 0xf) + 0x10;
      result = (I816_R(a) & 0xf0) + (value & 0xf0) + result;
    } else {
      result = (I816_R(a) & 0xff) + value + I816_R(c);
    }
    I816_W(v, (I816_R(a) & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80));
    if(I816_R(d) && result > 0x9f) result += 0x60;
    I816_W(c, result > 0xff);
    I816_W(a, (I816_R(a) & 0xff00) | (result & 0xff));
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high);
    int result = 0;
    if(I816_R(d)) {
      result = (I816_R(a) & 0xf) + (value & 0xf) + I816_R(c);
      if(result > 0x9) result = ((result + 0x6) & 0xf) + 0x10;
      result = (I816_R(a) & 0xf0) + (value & 0xf0) + result;
      if(result > 0x9f) result = ((result + 0x60) & 0xff) + 0x100;
      result = (I816_R(a) & 0xf00) + (value & 0xf00) + result;
      if(result > 0x9ff) result = ((result + 0x600) & 0xfff) + 0x1000;
      result = (I816_R(a) & 0xf000) + (value & 0xf000) + result;
    } else {
      result = I816_R(a) + value + I816_R(c);
    }
    I816_W(v, (I816_R(a) & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000));
    if(I816_R(d) && result > 0x9fff) result += 0x6000;
    I816_W(c, result > 0xffff);
    I816_W(a, result);
  }
  interp816_setZN(cpu, I816_R(a), I816_R(mf));
}

static void interp816_sbc(Interp816* cpu, uint32_t low, uint32_t high) {
  if(I816_R(mf)) {
    uint8_t value = interp816_read(cpu, low) ^ 0xff;
    int result = 0;
    if(I816_R(d)) {
      result = (I816_R(a) & 0xf) + (value & 0xf) + I816_R(c);
      if(result < 0x10) result = (result - 0x6) & ((result - 0x6 < 0) ? 0xf : 0x1f);
      result = (I816_R(a) & 0xf0) + (value & 0xf0) + result;
    } else {
      result = (I816_R(a) & 0xff) + value + I816_R(c);
    }
    I816_W(v, (I816_R(a) & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80));
    if(I816_R(d) && result < 0x100) result -= 0x60;
    I816_W(c, result > 0xff);
    I816_W(a, (I816_R(a) & 0xff00) | (result & 0xff));
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high) ^ 0xffff;
    int result = 0;
    if(I816_R(d)) {
      result = (I816_R(a) & 0xf) + (value & 0xf) + I816_R(c);
      if(result < 0x10) result = (result - 0x6) & ((result - 0x6 < 0) ? 0xf : 0x1f);
      result = (I816_R(a) & 0xf0) + (value & 0xf0) + result;
      if(result < 0x100) result = (result - 0x60) & ((result - 0x60 < 0) ? 0xff : 0x1ff);
      result = (I816_R(a) & 0xf00) + (value & 0xf00) + result;
      if(result < 0x1000) result = (result - 0x600) & ((result - 0x600 < 0) ? 0xfff : 0x1fff);
      result = (I816_R(a) & 0xf000) + (value & 0xf000) + result;
    } else {
      result = I816_R(a) + value + I816_R(c);
    }
    I816_W(v, (I816_R(a) & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000));
    if(I816_R(d) && result < 0x10000) result -= 0x6000;
    I816_W(c, result > 0xffff);
    I816_W(a, result);
  }
  interp816_setZN(cpu, I816_R(a), I816_R(mf));
}





















/* Opcodes that are an addressing mode followed by an operation: their
 * mode and operation, so interp816_doOpcode dispatches them through two
 * small switches instead of one switch case each (176 cases whose call
 * sequences and epilogues the compiler duplicated: 7 KB of code, a
 * different few lines per opcode in the I-cache). 0: a case of its own.
 * Generated from the per-opcode cases they replace; the helpers called,
 * and their order, are unchanged. */
enum { I816_M_NONE, I816_M_IDX, I816_M_SR, I816_M_DP, I816_M_IDL, I816_M_IMM_FALSE, I816_M_ABS, I816_M_ABL, I816_M_IDY_FALSE, I816_M_IDP, I816_M_ISY, I816_M_DPX, I816_M_ILY, I816_M_ABY_FALSE, I816_M_ABX_FALSE, I816_M_ABX_TRUE, I816_M_ALX, I816_M_IDY_TRUE, I816_M_DPY, I816_M_ABY_TRUE, I816_M_IMM_TRUE };
enum { I816_O_NONE, I816_O_ORA, I816_O_TSB, I816_O_ASL, I816_O_TRB, I816_O_AND, I816_O_BIT, I816_O_ROL, I816_O_EOR, I816_O_LSR, I816_O_ADC, I816_O_STZ, I816_O_ROR, I816_O_STA, I816_O_STY, I816_O_STX, I816_O_LDY, I816_O_LDA, I816_O_LDX, I816_O_CPY, I816_O_CMP, I816_O_DEC, I816_O_CPX, I816_O_SBC, I816_O_INC };
#ifdef I816_MAIN
extern const uint8_t k_i816_mode[256];
#else
const uint8_t k_i816_mode[256] = {
   0,  1,  0,  2,  3,  3,  3,  4,  0,  5,  0,  0,  6,  6,  6,  7,  /* 00 */
   0,  8,  9, 10,  3, 11, 11, 12,  0, 13,  0,  0,  6, 14, 15, 16,  /* 10 */
   0,  1,  0,  2,  3,  3,  3,  4,  0,  5,  0,  0,  6,  6,  6,  7,  /* 20 */
   0,  8,  9, 10, 11, 11, 11, 12,  0, 13,  0,  0, 14, 14, 15, 16,  /* 30 */
   0,  1,  0,  2,  0,  3,  3,  4,  0,  5,  0,  0,  0,  6,  6,  7,  /* 40 */
   0,  8,  9, 10,  0, 11, 11, 12,  0, 13,  0,  0,  0, 14, 15, 16,  /* 50 */
   0,  1,  0,  2,  3,  3,  3,  4,  0,  5,  0,  0,  0,  6,  6,  7,  /* 60 */
   0,  8,  9, 10, 11, 11, 11, 12,  0, 13,  0,  0,  0, 14, 15, 16,  /* 70 */
   0,  1,  0,  2,  3,  3,  3,  4,  0,  0,  0,  0,  6,  6,  6,  7,  /* 80 */
   0, 17,  9, 10, 11, 11, 18, 12,  0, 19,  0,  0,  6, 15, 15, 16,  /* 90 */
  20,  1, 20,  2,  3,  3,  3,  4,  0,  5,  0,  0,  6,  6,  6,  7,  /* A0 */
   0,  8,  9, 10, 11, 11, 18, 12,  0, 13,  0,  0, 14, 14, 13, 16,  /* B0 */
  20,  1,  0,  2,  3,  3,  3,  4,  0,  5,  0,  0,  6,  6,  6,  7,  /* C0 */
   0,  8,  9, 10,  0, 11, 11, 12,  0, 13,  0,  0,  0, 14, 15, 16,  /* D0 */
  20,  1,  0,  2,  3,  3,  3,  4,  0,  5,  0,  0,  6,  6,  6,  7,  /* E0 */
   0,  8,  9, 10,  0, 11, 11, 12,  0, 13,  0,  0,  0, 14, 15, 16,  /* F0 */
};
#endif
#ifdef I816_MAIN
extern const uint8_t k_i816_op[256];
#else
const uint8_t k_i816_op[256] = {
   0,  1,  0,  1,  2,  1,  3,  1,  0,  1,  0,  0,  2,  1,  3,  1,  /* 00 */
   0,  1,  1,  1,  4,  1,  3,  1,  0,  1,  0,  0,  4,  1,  3,  1,  /* 10 */
   0,  5,  0,  5,  6,  5,  7,  5,  0,  5,  0,  0,  6,  5,  7,  5,  /* 20 */
   0,  5,  5,  5,  6,  5,  7,  5,  0,  5,  0,  0,  6,  5,  7,  5,  /* 30 */
   0,  8,  0,  8,  0,  8,  9,  8,  0,  8,  0,  0,  0,  8,  9,  8,  /* 40 */
   0,  8,  8,  8,  0,  8,  9,  8,  0,  8,  0,  0,  0,  8,  9,  8,  /* 50 */
   0, 10,  0, 10, 11, 10, 12, 10,  0, 10,  0,  0,  0, 10, 12, 10,  /* 60 */
   0, 10, 10, 10, 11, 10, 12, 10,  0, 10,  0,  0,  0, 10, 12, 10,  /* 70 */
   0, 13,  0, 13, 14, 13, 15, 13,  0,  0,  0,  0, 14, 13, 15, 13,  /* 80 */
   0, 13, 13, 13, 14, 13, 15, 13,  0, 13,  0,  0, 11, 13, 11, 13,  /* 90 */
  16, 17, 18, 17, 16, 17, 18, 17,  0, 17,  0,  0, 16, 17, 18, 17,  /* A0 */
   0, 17, 17, 17, 16, 17, 18, 17,  0, 17,  0,  0, 16, 17, 18, 17,  /* B0 */
  19, 20,  0, 20, 19, 20, 21, 20,  0, 20,  0,  0, 19, 20, 21, 20,  /* C0 */
   0, 20, 20, 20,  0, 20, 21, 20,  0, 20,  0,  0,  0, 20, 21, 20,  /* D0 */
  22, 23,  0, 23, 22, 23, 24, 23,  0, 23,  0,  0, 22, 23, 24, 23,  /* E0 */
   0, 23, 23, 23,  0, 23, 24, 23,  0, 23,  0,  0,  0, 23, 24, 23,  /* F0 */
};
#endif

static void interp816_doOpcode(Interp816* cpu, uint8_t opcode) {
  switch(opcode) {
    case 0x00: { // brk imp
      if(cpu->brkHookEnabled) {
        /* `brkHookEnabled` dates from a bridge that bounced through PLANTED
         * BRK markers, where executing one had to be inert. Nothing plants
         * them any more -- interp_bridge.c states it outright: "The bounce is
         * via explicit JSR/JSL interception below, not via planted BRKs" --
         * so the only BRK the main CPU can now execute is one the guest
         * really reached, which means it is off the rails.
         *
         * Swallowing it was actively harmful. Hardware vectors on the FIRST
         * $00; this continued one byte at a time, so an off-rails run slid
         * through blank memory, crossed back into real code and corrupted the
         * stack before anything trapped. Super Metroid's Morph Ball freeze
         * (docs/validation/bringup-history.md, 2026-09-19) executed 240 such bytes and only
         * stopped on an unrelated COP ~9,000 steps later, by which point the
         * instruction ring held nothing but the slide and the original bad
         * jump was unrecoverable.
         *
         * So: report it once per run and then vector architecturally, which
         * is both faithful and what stops an off-rails guest AT its fault.
         * The flag is kept for callers that set it explicitly (SA-1 clears
         * it) and no longer suppresses the vector. */
        static int s_brk_reports;
        if (s_brk_reports < 8) {
          s_brk_reports++;
          fprintf(stderr,
                  "[brk] architectural BRK at $%02X:%04X (m=%d x=%d e=%d "
                  "sp=$%04X) -- vectoring; a guest that reaches BRK is "
                  "off the rails\n",
                  I816_R(k), (unsigned)(cpu->pc - 1), (int)I816_R(mf), (int)I816_R(xf),
                  (int)I816_R(e), (unsigned)I816_R(sp));
        }
      }
      {
        /* BRK is a two-byte instruction. The signature byte is fetched before
         * the return address is stacked. */
        interp816_readOpcode(cpu);
        if(!I816_R(e)) interp816_pushByte(cpu, I816_R(k));
        interp816_pushWord(cpu, cpu->pc);
        interp816_pushByte(cpu, interp816_getFlags(cpu) | (I816_R(e) ? 0x10 : 0));
        if(!I816_R(e)) cpu->cyclesUsed++; // native mode: 1 extra cycle (as COP)
        I816_W(i, true);
        I816_W(d, false);
        I816_W(k, 0);
        cpu->pc = I816_R(e)
            ? interp816_readWord(cpu, 0xfffe, 0xffff)
            : interp816_readWord(cpu, 0xffe6, 0xffe7);
      }
      break;
    }
    case 0x02: { // cop imm(s)
      interp816_readOpcode(cpu);
      interp816_pushByte(cpu, I816_R(k));
      interp816_pushWord(cpu, cpu->pc);
      interp816_pushByte(cpu, interp816_getFlags(cpu));
      cpu->cyclesUsed++; // native mode: 1 extra cycle
      I816_W(i, true);
      I816_W(d, false);
      I816_W(k, 0);
      cpu->pc = interp816_readWord(cpu, 0xffe4, 0xffe5);
      break;
    }
    case 0x0a: { // asla imp
      if(I816_R(mf)) {
        I816_W(c, I816_R(a) & 0x80);
        I816_W(a, (I816_R(a) & 0xff00) | ((I816_R(a) << 1) & 0xff));
      } else {
        I816_W(c, I816_R(a) & 0x8000);
        I816_W(a, I816_R(a) << (1));
      }
      interp816_setZN(cpu, I816_R(a), I816_R(mf));
      break;
    }
    case 0x0b: { // phd imp
      interp816_pushWord(cpu, I816_R(dp));
      break;
    }
    case 0x1b: { // tcs imp
      I816_W(sp, I816_R(a));
      break;
    }
    case 0x20: { // jsr abs
      uint16_t value = interp816_readOpcodeWord(cpu);
      interp816_pushWord(cpu, cpu->pc - 1);
      cpu->pc = value;
      break;
    }
    case 0x22: { // jsl abl
      uint16_t value = interp816_readOpcodeWord(cpu);
      uint8_t newK = interp816_readOpcode(cpu);
      interp816_pushByte(cpu, I816_R(k));
      interp816_pushWord(cpu, cpu->pc - 1);
      cpu->pc = value;
      /* Do not mask PB to 7 bits. JSL $9E:xxxx must keep K=$9E so PHK/PLB
       * sets DB into the $80-$BF register window (Metal Warriors Konami). */
      I816_W(k, newK);
      break;
    }
    case 0x28: { // plp imp
      interp816_setFlags(cpu, interp816_pullByte(cpu));
      break;
    }
    case 0x2a: { // rola imp
      int result = (I816_R(a) << 1) | I816_R(c);
      if(I816_R(mf)) {
        I816_W(c, result & 0x100);
        I816_W(a, (I816_R(a) & 0xff00) | (result & 0xff));
      } else {
        I816_W(c, result & 0x10000);
        I816_W(a, result);
      }
      interp816_setZN(cpu, I816_R(a), I816_R(mf));
      break;
    }
    case 0x2b: { // pld imp
      I816_W(dp, interp816_pullWord(cpu));
      interp816_setZN(cpu, I816_R(dp), false);
      break;
    }
    case 0x3b: { // tsc imp
      I816_W(a, I816_R(sp));
      interp816_setZN(cpu, I816_R(a), false);
      break;
    }
    case 0x40: { // rti imp
      interp816_setFlags(cpu, interp816_pullByte(cpu));
      cpu->cyclesUsed++; // native mode: 1 extra cycle
      cpu->pc = interp816_pullWord(cpu);
      I816_W(k, interp816_pullByte(cpu));
      break;
    }
    case 0x42: { // wdm imm(s)
      interp816_readOpcode(cpu);
      break;
    }
    case 0x44: { // mvp bm
      uint8_t dest = interp816_readOpcode(cpu);
      uint8_t src = interp816_readOpcode(cpu);
      I816_W(db, dest);
      interp816_write(cpu, (dest << 16) | I816_R(y), interp816_read(cpu, (src << 16) | I816_R(x)));
      I816_W(a, I816_R(a) - 1);
      I816_W(x, I816_R(x) - 1);
      I816_W(y, I816_R(y) - 1);
      if(I816_R(a) != 0xffff) {
        cpu->pc -= 3;
      }
      if(I816_R(xf)) {
        I816_W(x, I816_R(x) & (0xff));
        I816_W(y, I816_R(y) & (0xff));
      }
      break;
    }
    case 0x4a: { // lsra imp
      I816_W(c, I816_R(a) & 1);
      if(I816_R(mf)) {
        I816_W(a, (I816_R(a) & 0xff00) | ((I816_R(a) >> 1) & 0x7f));
      } else {
        I816_W(a, I816_R(a) >> (1));
      }
      interp816_setZN(cpu, I816_R(a), I816_R(mf));
      break;
    }
    case 0x4c: { // jmp abs
      cpu->pc = interp816_readOpcodeWord(cpu);
      break;
    }
    case 0x54: { // mvn bm
      uint8_t dest = interp816_readOpcode(cpu);
      uint8_t src = interp816_readOpcode(cpu);
      I816_W(db, dest);
      interp816_write(cpu, (dest << 16) | I816_R(y), interp816_read(cpu, (src << 16) | I816_R(x)));
      I816_W(a, I816_R(a) - 1);
      I816_W(x, I816_R(x) + 1);
      I816_W(y, I816_R(y) + 1);
      if(I816_R(a) != 0xffff) {
        cpu->pc -= 3;
      }
      if(I816_R(xf)) {
        I816_W(x, I816_R(x) & (0xff));
        I816_W(y, I816_R(y) & (0xff));
      }
      break;
    }
    case 0x5b: { // tcd imp
      I816_W(dp, I816_R(a));
      interp816_setZN(cpu, I816_R(dp), false);
      break;
    }
    case 0x5c: { // jml abl
      uint16_t value = interp816_readOpcodeWord(cpu);
      uint8_t new_k = interp816_readOpcode(cpu);
      I816_W(k, new_k);
      cpu->pc = value;
      break;
    }
    case 0x60: { // rts imp
      cpu->pc = interp816_pullWord(cpu) + 1;
      break;
    }
    case 0x62: { // per rll
      uint16_t value = interp816_readOpcodeWord(cpu);
      interp816_pushWord(cpu, cpu->pc + (int16_t) value);
      break;
    }
    case 0x6a: { // rora imp
      bool carry = I816_R(a) & 1;
      if(I816_R(mf)) {
        I816_W(a, (I816_R(a) & 0xff00) | ((I816_R(a) >> 1) & 0x7f) | (I816_R(c) << 7));
      } else {
        I816_W(a, (I816_R(a) >> 1) | (I816_R(c) << 15));
      }
      I816_W(c, carry);
      interp816_setZN(cpu, I816_R(a), I816_R(mf));
      break;
    }
    case 0x6b: { // rtl imp
      cpu->pc = interp816_pullWord(cpu) + 1;
      I816_W(k, interp816_pullByte(cpu));
      break;
    }
    case 0x6c: { // jmp ind
      uint16_t adr = interp816_readOpcodeWord(cpu);
      cpu->pc = interp816_readWord(cpu, adr, (adr + 1) & 0xffff);
      break;
    }
    case 0x7b: { // tdc imp
      I816_W(a, I816_R(dp));
      interp816_setZN(cpu, I816_R(a), false);
      break;
    }
    case 0x7c: { // jmp iax
      cpu->pc = interp816_adrIax(cpu);
      break;
    }
    case 0x80: { // bra rel
      uint8_t offset = interp816_readOpcode(cpu);
      cpu->pc += (int8_t) offset;
      break;
    }
    case 0x82: { // brl rll
      uint16_t offset = interp816_readOpcodeWord(cpu);
      cpu->pc += (int16_t) offset;
      break;
    }
    case 0x89: { // biti imm(m)
      if(I816_R(mf)) {
        uint8_t result = (I816_R(a) & 0xff) & interp816_readOpcode(cpu);
        I816_W(z, result == 0);
      } else {
        cpu->cyclesUsed++; // m = 0: 1 extra cycle
        uint16_t result = I816_R(a) & interp816_readOpcodeWord(cpu);
        I816_W(z, result == 0);
      }
      break;
    }
    case 0x9a: { // txs imp
      I816_W(sp, I816_R(x));
      break;
    }
    case 0xab: { // plb imp
      I816_W(db, interp816_pullByte(cpu));
      interp816_setZN(cpu, I816_R(db), true);
      break;
    }
    case 0xc2: { // rep imm(s)
      interp816_setFlags(cpu, interp816_getFlags(cpu) & ~interp816_readOpcode(cpu));
      break;
    }
    case 0xcb: { // wai imp
      cpu->waiting = true;
      break;
    }
    case 0xd4: { // pei dp
      uint32_t low = 0;
      uint32_t high = interp816_adrDp(cpu, &low);
      interp816_pushWord(cpu, interp816_readWord(cpu, low, high));
      break;
    }
    case 0xdb: { // stp imp
      cpu->stopped = true;
      break;
    }
    case 0xdc: { // jml ial
      uint16_t adr = interp816_readOpcodeWord(cpu);
      cpu->pc = interp816_readWord(cpu, adr, (adr + 1) & 0xffff);
      I816_W(k, interp816_read(cpu, (adr + 2) & 0xffff));
      break;
    }
    case 0xe2: { // sep imm(s)
      interp816_setFlags(cpu, interp816_getFlags(cpu) | interp816_readOpcode(cpu));
      break;
    }
    case 0xea: { // nop imp
      // no operation
      break;
    }
    case 0xeb: { // xba imp
      uint8_t low = I816_R(a) & 0xff;
      uint8_t high = I816_R(a) >> 8;
      I816_W(a, (low << 8) | high);
      interp816_setZN(cpu, high, true);
      break;
    }
    case 0xf4: { // pea imm(l)
      uint16_t w = interp816_readOpcodeWord(cpu);
      if (w == 0xC2AE)
        I816_W(y, 0);
      interp816_pushWord(cpu, w);
  
      break;
    }
    case 0xfb: { // xce imp
      bool temp = I816_R(c);
      I816_W(c, I816_R(e));
      I816_W(e, temp);
      interp816_setFlags(cpu, interp816_getFlags(cpu)); // updates x and m flags, clears upper half of x and y if needed
      break;
    }
    case 0xfc: { // jsr iax
      uint16_t value = interp816_adrIax(cpu);
      interp816_pushWord(cpu, cpu->pc - 1);
      cpu->pc = value;
      break;
    }

    /* Families of opcodes that differ only in the register or flag they use
     * (selected from the opcode): one case each, same helpers in the same
     * order as the per-opcode cases they replace. */
    case 0x10: case 0x30: case 0x50: case 0x70:
    case 0x90: case 0xb0: case 0xd0: case 0xf0: { // bpl bmi bvc bvs bcc bcs bne beq
      const uint8_t sel = opcode >> 6;   /* N, V, C, Z; taken when it equals bit 5 */
      const bool flag = sel == 0 ? I816_R(n) : sel == 1 ? I816_R(v) : sel == 2 ? I816_R(c) : I816_R(z);
      interp816_doBranch(cpu, interp816_readOpcode(cpu), flag == ((opcode >> 5) & 1));
      break;
    }
    case 0x18: case 0x38: case 0x58: case 0x78:
    case 0xb8: case 0xd8: case 0xf8: { // clc sec cli sei clv cld sed
      const bool f = (opcode & 0x20) && opcode != 0xb8;
      if (opcode < 0x40) I816_W(c, f); else if (opcode < 0x80) I816_W(i, f);
      else if (opcode < 0xc0) I816_W(v, f); else I816_W(d, f);
      break;
    }
    case 0xa8: case 0xaa: case 0x9b: case 0xbb: case 0xba: { // tay tax txy tyx tsx
      const uint16_t src = opcode == 0xba ? I816_R(sp) : opcode == 0x9b ? I816_R(x) :
                           opcode == 0xbb ? I816_R(y) : I816_R(a);
      const uint16_t dst = I816_R(xf) ? (src & 0xff) : src;
      if (opcode == 0xa8 || opcode == 0x9b) I816_W(y, dst); else I816_W(x, dst);
      interp816_setZN(cpu, dst, I816_R(xf));
      break;
    }
    case 0xe8: case 0xc8: case 0xca: case 0x88: { // inx iny dex dey
      const bool isx = opcode == 0xe8 || opcode == 0xca;
      const uint16_t r0 = isx ? I816_R(x) : I816_R(y);
      const uint16_t v = (opcode == 0xe8 || opcode == 0xc8) ? (uint16_t)(r0 + 1) : (uint16_t)(r0 - 1);
      const uint16_t r = I816_R(xf) ? (v & 0xff) : v;
      if (isx) I816_W(x, r); else I816_W(y, r);
      interp816_setZN(cpu, r, I816_R(xf));
      break;
    }
    case 0x8a: case 0x98: case 0x1a: case 0x3a: { // txa tya inca deca
      const uint16_t v = opcode == 0x8a ? I816_R(x) : opcode == 0x98 ? I816_R(y) :
                         opcode == 0x1a ? (uint16_t)(I816_R(a) + 1) : (uint16_t)(I816_R(a) - 1);
      I816_W(a, I816_R(mf) ? (uint16_t)((I816_R(a) & 0xff00) | (v & 0xff)) : v);
      interp816_setZN(cpu, I816_R(a), I816_R(mf));
      break;
    }
    case 0x48: case 0xda: case 0x5a: { // pha phx phy
      const uint16_t v = opcode == 0x48 ? I816_R(a) : opcode == 0xda ? I816_R(x) : I816_R(y);
      if(opcode == 0x48 ? I816_R(mf) : I816_R(xf)) {
        interp816_pushByte(cpu, (uint8_t)v);
      } else {
        cpu->cyclesUsed++; // 16-bit: 1 extra cycle
        interp816_pushWord(cpu, v);
      }
      break;
    }
    case 0x68: case 0xfa: case 0x7a: { // pla plx ply
      const bool byte = opcode == 0x68 ? I816_R(mf) : I816_R(xf);
      uint16_t r;
      if(byte) {
        /* 8-bit A keeps its high byte; 8-bit X and Y have none */
        r = (opcode == 0x68 ? (I816_R(a) & 0xff00) : 0) | interp816_pullByte(cpu);
      } else {
        cpu->cyclesUsed++; // 16-bit: 1 extra cycle
        r = interp816_pullWord(cpu);
      }
      if (opcode == 0x68) I816_W(a, r); else if (opcode == 0xfa) I816_W(x, r); else I816_W(y, r);
      interp816_setZN(cpu, r, byte);
      break;
    }
    case 0x08: case 0x4b: case 0x8b: { // php phk phb
      interp816_pushByte(cpu, opcode == 0x08 ? interp816_getFlags(cpu) : opcode == 0x4b ? I816_R(k) : I816_R(db));
      break;
    }    default: {  /* addressing mode + operation (k_i816_mode, k_i816_op) */
      uint32_t low = 0, high = 0;
      switch (k_i816_mode[opcode]) {
        case I816_M_IDX: high = interp816_adrIdx(cpu, &low); break;
        case I816_M_SR: high = interp816_adrSr(cpu, &low); break;
        case I816_M_DP: high = interp816_adrDp(cpu, &low); break;
        case I816_M_IDL: high = interp816_adrIdl(cpu, &low); break;
        case I816_M_IMM_FALSE: high = interp816_adrImm(cpu, &low, false); break;
        case I816_M_ABS: high = interp816_adrAbs(cpu, &low); break;
        case I816_M_ABL: high = interp816_adrAbl(cpu, &low); break;
        case I816_M_IDY_FALSE: high = interp816_adrIdy(cpu, &low, false); break;
        case I816_M_IDP: high = interp816_adrIdp(cpu, &low); break;
        case I816_M_ISY: high = interp816_adrIsy(cpu, &low); break;
        case I816_M_DPX: high = interp816_adrDpx(cpu, &low); break;
        case I816_M_ILY: high = interp816_adrIly(cpu, &low); break;
        case I816_M_ABY_FALSE: high = interp816_adrAby(cpu, &low, false); break;
        case I816_M_ABX_FALSE: high = interp816_adrAbx(cpu, &low, false); break;
        case I816_M_ABX_TRUE: high = interp816_adrAbx(cpu, &low, true); break;
        case I816_M_ALX: high = interp816_adrAlx(cpu, &low); break;
        case I816_M_IDY_TRUE: high = interp816_adrIdy(cpu, &low, true); break;
        case I816_M_DPY: high = interp816_adrDpy(cpu, &low); break;
        case I816_M_ABY_TRUE: high = interp816_adrAby(cpu, &low, true); break;
        case I816_M_IMM_TRUE: high = interp816_adrImm(cpu, &low, true); break;
        default: I816_UNREACHABLE();
      }
      /* Operations of one shape share their code across widths: M (or X for
       * the index registers) selects 8 or 16 bits, m is its mask. ADC and SBC
       * keep their functions (decimal mode). Expressions, bus accesses and
       * cycle charges are those of the per-operation functions they replace. */
      const uint8_t op = k_i816_op[opcode];
      switch (op) {
        case I816_O_ADC: interp816_adc(cpu, low, high); break;
        case I816_O_SBC: interp816_sbc(cpu, low, high); break;
        case I816_O_STA: case I816_O_STX: case I816_O_STY: case I816_O_STZ: {
          const bool byte = (op == I816_O_STX || op == I816_O_STY) ? I816_R(xf) : I816_R(mf);
          const uint16_t v = op == I816_O_STA ? I816_R(a) : op == I816_O_STX ? I816_R(x) :
                             op == I816_O_STY ? I816_R(y) : 0;
          if(byte) {
            interp816_write(cpu, low, (uint8_t)v);
          } else {
            cpu->cyclesUsed++; // 16-bit: 1 extra cycle
            interp816_writeWord(cpu, low, high, v, false);
          }
          break;
        }
        case I816_O_ASL: case I816_O_LSR: case I816_O_ROL: case I816_O_ROR:
        case I816_O_INC: case I816_O_DEC: case I816_O_TSB: case I816_O_TRB: {
          const bool byte = I816_R(mf);
          const int m = byte ? 0xff : 0xffff;
          uint16_t value;
          if(byte) {
            value = interp816_read(cpu, low);
          } else {
            cpu->cyclesUsed += 2; // 16-bit: 2 extra cycles
            value = interp816_readWord(cpu, low, high);
          }
          int result;
          bool flags = true;   /* N and Z from the result (not TSB/TRB) */
          switch (op) {
            case I816_O_ASL: result = value << 1; I816_W(c, result & (m + 1)); break;
            case I816_O_ROL: result = (value << 1) | I816_R(c); I816_W(c, result & (m + 1)); break;
            case I816_O_LSR: I816_W(c, value & 1); result = value >> 1; break;
            case I816_O_ROR: {
              const bool carry = value & 1;
              result = (value >> 1) | (I816_R(c) << (byte ? 7 : 15));
              if(byte) interp816_write(cpu, low, result);
              else interp816_writeWord(cpu, low, high, result, true);
              interp816_setZN(cpu, result, byte);
              I816_W(c, carry);
              flags = false;
              goto rmw_done;
            }
            case I816_O_INC: result = value + 1; break;
            case I816_O_DEC: result = value - 1; break;
            case I816_O_TSB:
              I816_W(z, ((I816_R(a) & m) & value) == 0);
              result = value | (I816_R(a) & m); flags = false; break;
            default: /* TRB */
              I816_W(z, ((I816_R(a) & m) & value) == 0);
              result = value & ~(I816_R(a) & m); flags = false; break;
          }
          if(byte) interp816_write(cpu, low, result);
          else interp816_writeWord(cpu, low, high, result, true);
          if(flags) interp816_setZN(cpu, result, byte);
        rmw_done:
          break;
        }
        default: {  /* read an operand: AND ORA EOR CMP CPX CPY BIT LDA LDX LDY */
          const bool xop = op == I816_O_CPX || op == I816_O_CPY || op == I816_O_LDX || op == I816_O_LDY;
          const bool byte = xop ? I816_R(xf) : I816_R(mf);
          const uint16_t m = byte ? 0xff : 0xffff;
          uint16_t value;
          if(byte) {
            value = interp816_read(cpu, low);
          } else {
            cpu->cyclesUsed++; // 16-bit: 1 extra cycle
            value = interp816_readWord(cpu, low, high);
          }
          switch (op) {
            case I816_O_AND: I816_W(a, (I816_R(a) & ~m) | ((I816_R(a) & value) & m)); interp816_setZN(cpu, I816_R(a), byte); break;
            case I816_O_ORA: I816_W(a, (I816_R(a) & ~m) | ((I816_R(a) | value) & m)); interp816_setZN(cpu, I816_R(a), byte); break;
            case I816_O_EOR: I816_W(a, (I816_R(a) & ~m) | ((I816_R(a) ^ value) & m)); interp816_setZN(cpu, I816_R(a), byte); break;
            case I816_O_LDA: I816_W(a, (I816_R(a) & ~m) | value); interp816_setZN(cpu, I816_R(a), byte); break;
            case I816_O_LDX: I816_W(x, value); interp816_setZN(cpu, I816_R(x), byte); break;
            case I816_O_LDY: I816_W(y, value); interp816_setZN(cpu, I816_R(y), byte); break;
            case I816_O_BIT:
              I816_W(z, ((I816_R(a) & m) & value) == 0);
              I816_W(n, value & (byte ? 0x80 : 0x8000));
              I816_W(v, value & (byte ? 0x40 : 0x4000));
              break;
            default: {  /* CMP CPX CPY */
              const uint16_t reg = op == I816_O_CPX ? I816_R(x) : op == I816_O_CPY ? I816_R(y) : I816_R(a);
              const int result = (reg & m) + (value ^ m) + 1;
              I816_W(c, result > m);
              interp816_setZN(cpu, result, byte);
              break;
            }
          }
          break;
        }
      }
      break;
    }
  }
}
