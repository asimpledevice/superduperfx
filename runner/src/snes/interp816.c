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

static const int cyclesPerOpcode[256] = {
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

void interp816_reset(Interp816* cpu) {
  cpu->a = 0;
  cpu->x = 0;
  cpu->y = 0;
  cpu->sp = 0x100;
  cpu->pc = interp816_read(cpu, 0xfffc) | (interp816_read(cpu, 0xfffd) << 8);
  cpu->dp = 0;
  cpu->k = 0;
  cpu->db = 0;
  cpu->c = false;
  cpu->z = false;
  cpu->v = false;
  cpu->n = false;
  cpu->i = true;
  cpu->d = false;
  cpu->xf = true;
  cpu->mf = true;
  cpu->e = true;
  cpu->irqWanted = false;
  cpu->nmiWanted = false;
  cpu->waiting = false;
  cpu->stopped = false;
  cpu->cyclesUsed = 0;
}

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

static uint64_t s_interp816_insns;
static uint64_t s_interp816_cycles;
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
static uint64_t s_interp_page_cycles[INTERP816_PAGE_COUNT];
static uint64_t s_interp_page_insns[INTERP816_PAGE_COUNT];

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
static uint32_t s_pc_buckets[INTERP_PC_BUCKETS];
static uint32_t s_pc24[INTERP_PC_BUCKETS];
static uint32_t s_pc_bucket_total = 0;
static uint32_t s_pc_sample_skip = 0;
static uint32_t s_oh_count[256], s_oh_tot[256]; /* per-opcode: samples, host-ns */
static uint32_t s_oh_sample; /* 1-in-64 probe counter */
static int s_oh_on = -1;
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

int interp816_runOpcode(Interp816* cpu) {
  cpu->cyclesUsed = 0;
  s_interp816_opcodes_run++;
  /* getenv() walks the whole environment block on MSVC (~us) - cache it so
   * the hot path pays a branch, not a CRT call, per interpreted instruction.
   * Same value as getenv, so behaviour is bit-identical. */
  {
    static int s_phase_ms = -1;
    if (s_phase_ms < 0) s_phase_ms = getenv("SNESRECOMP_PHASE_MS") ? 1 : 0;
    if (s_phase_ms) {
      if (++s_pc_sample_skip >= 8) {
          s_pc_sample_skip = 0;
          uint32_t pc = ((uint32_t)cpu->k << 16) | cpu->pc;
          uint32_t h = (pc * 2654435761u) >> (32 - 12);
          s_pc_buckets[h]++;
          s_pc24[h] = pc;
          s_pc_bucket_total++;
      }
    }
  }
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
  if(!cpu->i && cpu->irqWanted) {
    cpu->irqWanted = false;
    cpu->cyclesUsed = 7;
    interp816_doInterrupt(cpu, true);
    return cpu->cyclesUsed;
  }

  uint8_t opcode = interp816_readOpcode(cpu);
  uint32_t _pcb = ((uint32_t)cpu->k << 16) | (uint16_t)(cpu->pc - 1);
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
  return interp816_read(cpu, (cpu->k << 16) | cpu->pc++);
}

static uint16_t interp816_readOpcodeWord(Interp816* cpu) {
  uint8_t low = interp816_readOpcode(cpu);
  return low | (interp816_readOpcode(cpu) << 8);
}

uint8_t interp816_getFlags(Interp816* cpu) {
  uint8_t val = cpu->n << 7;
  val |= cpu->v << 6;
  val |= cpu->mf << 5;
  val |= cpu->xf << 4;
  val |= cpu->d << 3;
  val |= cpu->i << 2;
  val |= cpu->z << 1;
  val |= cpu->c;
  return val;
}

void interp816_setFlags(Interp816* cpu, uint8_t val) {
  cpu->n = val & 0x80;
  cpu->v = val & 0x40;
  cpu->mf = val & 0x20;
  cpu->xf = val & 0x10;
  cpu->d = val & 8;
  cpu->i = val & 4;
  cpu->z = val & 2;
  cpu->c = val & 1;
  if(cpu->e) {
    cpu->mf = true;
    cpu->xf = true;
    cpu->sp = (cpu->sp & 0xff) | 0x100;
  }
  if(cpu->xf) {
    cpu->x &= 0xff;
    cpu->y &= 0xff;
  }
}

static void interp816_setZN(Interp816* cpu, uint16_t value, bool byte) {
  if(byte) {
    cpu->z = (value & 0xff) == 0;
    cpu->n = value & 0x80;
  } else {
    cpu->z = value == 0;
    cpu->n = value & 0x8000;
  }
}

static void interp816_doBranch(Interp816* cpu, uint8_t value, bool check) {
  if(check) {
    cpu->cyclesUsed++; // taken branch: 1 extra cycle
    cpu->pc += (int8_t) value;
  }
}

static uint8_t interp816_pullByte(Interp816* cpu) {
  cpu->sp++;
  if(cpu->e) cpu->sp = (cpu->sp & 0xff) | 0x100;
  return interp816_read(cpu, cpu->sp);
}

static void interp816_pushByte(Interp816* cpu, uint8_t value) {
  interp816_write(cpu, cpu->sp, value);
  cpu->sp--;
  if(cpu->e) cpu->sp = (cpu->sp & 0xff) | 0x100;
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
  interp816_pushByte(cpu, cpu->k);
  interp816_pushWord(cpu, cpu->pc);
  interp816_pushByte(cpu, interp816_getFlags(cpu));
  cpu->cyclesUsed++; // native mode: 1 extra cycle
  cpu->i = true;
  cpu->d = false;
  cpu->k = 0;
  if(irq) {
    cpu->pc = interp816_readWord(cpu, 0xffee, 0xffef);
  } else {
    // nmi
    cpu->pc = interp816_readWord(cpu, 0xffea, 0xffeb);
  }
}

// addressing modes

static uint32_t interp816_adrImm(Interp816* cpu, uint32_t* low, bool xFlag) {
  if((xFlag && cpu->xf) || (!xFlag && cpu->mf)) {
    *low = (cpu->k << 16) | cpu->pc++;
    return 0;
  } else {
    *low = (cpu->k << 16) | cpu->pc++;
    return (cpu->k << 16) | cpu->pc++;
  }
}

static uint32_t interp816_adrDp(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  *low = (cpu->dp + adr) & 0xffff;
  return (cpu->dp + adr + 1) & 0xffff;
}

static uint32_t interp816_adrDpx(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  *low = (cpu->dp + adr + cpu->x) & 0xffff;
  return (cpu->dp + adr + cpu->x + 1) & 0xffff;
}

static uint32_t interp816_adrDpy(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  *low = (cpu->dp + adr + cpu->y) & 0xffff;
  return (cpu->dp + adr + cpu->y + 1) & 0xffff;
}

static uint32_t interp816_adrIdp(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint16_t pointer = interp816_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  *low = (cpu->db << 16) + pointer;
  return ((cpu->db << 16) + pointer + 1) & 0xffffff;
}

static uint32_t interp816_adrIdx(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint16_t pointer = interp816_readWord(cpu, (cpu->dp + adr + cpu->x) & 0xffff, (cpu->dp + adr + cpu->x + 1) & 0xffff);
  *low = (cpu->db << 16) + pointer;
  return ((cpu->db << 16) + pointer + 1) & 0xffffff;
}

static uint32_t interp816_adrIdy(Interp816* cpu, uint32_t* low, bool write) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint16_t pointer = interp816_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  bool crossed = (pointer >> 8) != ((pointer + cpu->y) >> 8);
  // W65C816S cycle-table note 4: read penalty for X=0 OR crossing.
  // Store/RMW base cycles already include the unconditional indexing cycle.
  if(!write && (!cpu->xf || crossed)) cpu->cyclesUsed++;
  *low = ((cpu->db << 16) + pointer + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + pointer + cpu->y + 1) & 0xffffff;
}

static uint32_t interp816_adrIdl(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint32_t pointer = interp816_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  pointer |= interp816_read(cpu, (cpu->dp + adr + 2) & 0xffff) << 16;
  *low = pointer;
  return (pointer + 1) & 0xffffff;
}

static uint32_t interp816_adrIly(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu->cyclesUsed++; // dpr not 0: 1 extra cycle
  uint32_t pointer = interp816_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  pointer |= interp816_read(cpu, (cpu->dp + adr + 2) & 0xffff) << 16;
  *low = (pointer + cpu->y) & 0xffffff;
  return (pointer + cpu->y + 1) & 0xffffff;
}

static uint32_t interp816_adrSr(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  *low = (cpu->sp + adr) & 0xffff;
  return (cpu->sp + adr + 1) & 0xffff;
}

static uint32_t interp816_adrIsy(Interp816* cpu, uint32_t* low) {
  uint8_t adr = interp816_readOpcode(cpu);
  uint16_t pointer = interp816_readWord(cpu, (cpu->sp + adr) & 0xffff, (cpu->sp + adr + 1) & 0xffff);
  *low = ((cpu->db << 16) + pointer + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + pointer + cpu->y + 1) & 0xffffff;
}

static uint32_t interp816_adrAbs(Interp816* cpu, uint32_t* low) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  *low = (cpu->db << 16) + adr;
  return ((cpu->db << 16) + adr + 1) & 0xffffff;
}

static uint32_t interp816_adrAbx(Interp816* cpu, uint32_t* low, bool write) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  bool crossed = (adr >> 8) != ((adr + cpu->x) >> 8);
  if(!write && (!cpu->xf || crossed)) cpu->cyclesUsed++;
  *low = ((cpu->db << 16) + adr + cpu->x) & 0xffffff;
  return ((cpu->db << 16) + adr + cpu->x + 1) & 0xffffff;
}

static uint32_t interp816_adrAby(Interp816* cpu, uint32_t* low, bool write) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  bool crossed = (adr >> 8) != ((adr + cpu->y) >> 8);
  if(!write && (!cpu->xf || crossed)) cpu->cyclesUsed++;
  *low = ((cpu->db << 16) + adr + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + adr + cpu->y + 1) & 0xffffff;
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
  *low = (adr + cpu->x) & 0xffffff;
  return (adr + cpu->x + 1) & 0xffffff;
}

static uint16_t interp816_adrIax(Interp816* cpu) {
  uint16_t adr = interp816_readOpcodeWord(cpu);
  return interp816_readWord(cpu, (cpu->k << 16) | ((adr + cpu->x) & 0xffff), (cpu->k << 16) | ((adr + cpu->x + 1) & 0xffff));
}

// opcode functions




static void interp816_adc(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low);
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result > 0x9) result = ((result + 0x6) & 0xf) + 0x10;
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
    } else {
      result = (cpu->a & 0xff) + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
    if(cpu->d && result > 0x9f) result += 0x60;
    cpu->c = result > 0xff;
    cpu->a = (cpu->a & 0xff00) | (result & 0xff);
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high);
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result > 0x9) result = ((result + 0x6) & 0xf) + 0x10;
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
      if(result > 0x9f) result = ((result + 0x60) & 0xff) + 0x100;
      result = (cpu->a & 0xf00) + (value & 0xf00) + result;
      if(result > 0x9ff) result = ((result + 0x600) & 0xfff) + 0x1000;
      result = (cpu->a & 0xf000) + (value & 0xf000) + result;
    } else {
      result = cpu->a + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
    if(cpu->d && result > 0x9fff) result += 0x6000;
    cpu->c = result > 0xffff;
    cpu->a = result;
  }
  interp816_setZN(cpu, cpu->a, cpu->mf);
}

static void interp816_sbc(Interp816* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = interp816_read(cpu, low) ^ 0xff;
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result < 0x10) result = (result - 0x6) & ((result - 0x6 < 0) ? 0xf : 0x1f);
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
    } else {
      result = (cpu->a & 0xff) + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
    if(cpu->d && result < 0x100) result -= 0x60;
    cpu->c = result > 0xff;
    cpu->a = (cpu->a & 0xff00) | (result & 0xff);
  } else {
    cpu->cyclesUsed++; // m = 0: 1 extra cycle
    uint16_t value = interp816_readWord(cpu, low, high) ^ 0xffff;
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result < 0x10) result = (result - 0x6) & ((result - 0x6 < 0) ? 0xf : 0x1f);
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
      if(result < 0x100) result = (result - 0x60) & ((result - 0x60 < 0) ? 0xff : 0x1ff);
      result = (cpu->a & 0xf00) + (value & 0xf00) + result;
      if(result < 0x1000) result = (result - 0x600) & ((result - 0x600 < 0) ? 0xfff : 0x1fff);
      result = (cpu->a & 0xf000) + (value & 0xf000) + result;
    } else {
      result = cpu->a + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
    if(cpu->d && result < 0x10000) result -= 0x6000;
    cpu->c = result > 0xffff;
    cpu->a = result;
  }
  interp816_setZN(cpu, cpu->a, cpu->mf);
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
static const uint8_t k_i816_mode[256] = {
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
static const uint8_t k_i816_op[256] = {
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
                  cpu->k, (unsigned)(cpu->pc - 1), (int)cpu->mf, (int)cpu->xf,
                  (int)cpu->e, (unsigned)cpu->sp);
        }
      }
      {
        /* BRK is a two-byte instruction. The signature byte is fetched before
         * the return address is stacked. */
        interp816_readOpcode(cpu);
        if(!cpu->e) interp816_pushByte(cpu, cpu->k);
        interp816_pushWord(cpu, cpu->pc);
        interp816_pushByte(cpu, interp816_getFlags(cpu) | (cpu->e ? 0x10 : 0));
        if(!cpu->e) cpu->cyclesUsed++; // native mode: 1 extra cycle (as COP)
        cpu->i = true;
        cpu->d = false;
        cpu->k = 0;
        cpu->pc = cpu->e
            ? interp816_readWord(cpu, 0xfffe, 0xffff)
            : interp816_readWord(cpu, 0xffe6, 0xffe7);
      }
      break;
    }
    case 0x02: { // cop imm(s)
      interp816_readOpcode(cpu);
      interp816_pushByte(cpu, cpu->k);
      interp816_pushWord(cpu, cpu->pc);
      interp816_pushByte(cpu, interp816_getFlags(cpu));
      cpu->cyclesUsed++; // native mode: 1 extra cycle
      cpu->i = true;
      cpu->d = false;
      cpu->k = 0;
      cpu->pc = interp816_readWord(cpu, 0xffe4, 0xffe5);
      break;
    }
    case 0x0a: { // asla imp
      if(cpu->mf) {
        cpu->c = cpu->a & 0x80;
        cpu->a = (cpu->a & 0xff00) | ((cpu->a << 1) & 0xff);
      } else {
        cpu->c = cpu->a & 0x8000;
        cpu->a <<= 1;
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x0b: { // phd imp
      interp816_pushWord(cpu, cpu->dp);
      break;
    }
    case 0x1b: { // tcs imp
      cpu->sp = cpu->a;
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
      interp816_pushByte(cpu, cpu->k);
      interp816_pushWord(cpu, cpu->pc - 1);
      cpu->pc = value;
      /* Do not mask PB to 7 bits. JSL $9E:xxxx must keep K=$9E so PHK/PLB
       * sets DB into the $80-$BF register window (Metal Warriors Konami). */
      cpu->k = newK;
      break;
    }
    case 0x28: { // plp imp
      interp816_setFlags(cpu, interp816_pullByte(cpu));
      break;
    }
    case 0x2a: { // rola imp
      int result = (cpu->a << 1) | cpu->c;
      if(cpu->mf) {
        cpu->c = result & 0x100;
        cpu->a = (cpu->a & 0xff00) | (result & 0xff);
      } else {
        cpu->c = result & 0x10000;
        cpu->a = result;
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x2b: { // pld imp
      cpu->dp = interp816_pullWord(cpu);
      interp816_setZN(cpu, cpu->dp, false);
      break;
    }
    case 0x3b: { // tsc imp
      cpu->a = cpu->sp;
      interp816_setZN(cpu, cpu->a, false);
      break;
    }
    case 0x40: { // rti imp
      interp816_setFlags(cpu, interp816_pullByte(cpu));
      cpu->cyclesUsed++; // native mode: 1 extra cycle
      cpu->pc = interp816_pullWord(cpu);
      cpu->k = interp816_pullByte(cpu);
      break;
    }
    case 0x42: { // wdm imm(s)
      interp816_readOpcode(cpu);
      break;
    }
    case 0x44: { // mvp bm
      uint8_t dest = interp816_readOpcode(cpu);
      uint8_t src = interp816_readOpcode(cpu);
      cpu->db = dest;
      interp816_write(cpu, (dest << 16) | cpu->y, interp816_read(cpu, (src << 16) | cpu->x));
      cpu->a--;
      cpu->x--;
      cpu->y--;
      if(cpu->a != 0xffff) {
        cpu->pc -= 3;
      }
      if(cpu->xf) {
        cpu->x &= 0xff;
        cpu->y &= 0xff;
      }
      break;
    }
    case 0x4a: { // lsra imp
      cpu->c = cpu->a & 1;
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | ((cpu->a >> 1) & 0x7f);
      } else {
        cpu->a >>= 1;
      }
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x4c: { // jmp abs
      cpu->pc = interp816_readOpcodeWord(cpu);
      break;
    }
    case 0x54: { // mvn bm
      uint8_t dest = interp816_readOpcode(cpu);
      uint8_t src = interp816_readOpcode(cpu);
      cpu->db = dest;
      interp816_write(cpu, (dest << 16) | cpu->y, interp816_read(cpu, (src << 16) | cpu->x));
      cpu->a--;
      cpu->x++;
      cpu->y++;
      if(cpu->a != 0xffff) {
        cpu->pc -= 3;
      }
      if(cpu->xf) {
        cpu->x &= 0xff;
        cpu->y &= 0xff;
      }
      break;
    }
    case 0x5b: { // tcd imp
      cpu->dp = cpu->a;
      interp816_setZN(cpu, cpu->dp, false);
      break;
    }
    case 0x5c: { // jml abl
      uint16_t value = interp816_readOpcodeWord(cpu);
      uint8_t new_k = interp816_readOpcode(cpu);
      cpu->k = new_k;
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
      bool carry = cpu->a & 1;
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | ((cpu->a >> 1) & 0x7f) | (cpu->c << 7);
      } else {
        cpu->a = (cpu->a >> 1) | (cpu->c << 15);
      }
      cpu->c = carry;
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x6b: { // rtl imp
      cpu->pc = interp816_pullWord(cpu) + 1;
      cpu->k = interp816_pullByte(cpu);
      break;
    }
    case 0x6c: { // jmp ind
      uint16_t adr = interp816_readOpcodeWord(cpu);
      cpu->pc = interp816_readWord(cpu, adr, (adr + 1) & 0xffff);
      break;
    }
    case 0x7b: { // tdc imp
      cpu->a = cpu->dp;
      interp816_setZN(cpu, cpu->a, false);
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
      if(cpu->mf) {
        uint8_t result = (cpu->a & 0xff) & interp816_readOpcode(cpu);
        cpu->z = result == 0;
      } else {
        cpu->cyclesUsed++; // m = 0: 1 extra cycle
        uint16_t result = cpu->a & interp816_readOpcodeWord(cpu);
        cpu->z = result == 0;
      }
      break;
    }
    case 0x9a: { // txs imp
      cpu->sp = cpu->x;
      break;
    }
    case 0xab: { // plb imp
      cpu->db = interp816_pullByte(cpu);
      interp816_setZN(cpu, cpu->db, true);
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
      cpu->k = interp816_read(cpu, (adr + 2) & 0xffff);
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
      uint8_t low = cpu->a & 0xff;
      uint8_t high = cpu->a >> 8;
      cpu->a = (low << 8) | high;
      interp816_setZN(cpu, high, true);
      break;
    }
    case 0xf4: { // pea imm(l)
      uint16_t w = interp816_readOpcodeWord(cpu);
      if (w == 0xC2AE)
        cpu->y = 0;
      interp816_pushWord(cpu, w);
  
      break;
    }
    case 0xfb: { // xce imp
      bool temp = cpu->c;
      cpu->c = cpu->e;
      cpu->e = temp;
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
      const bool flag = sel == 0 ? cpu->n : sel == 1 ? cpu->v : sel == 2 ? cpu->c : cpu->z;
      interp816_doBranch(cpu, interp816_readOpcode(cpu), flag == ((opcode >> 5) & 1));
      break;
    }
    case 0x18: case 0x38: case 0x58: case 0x78:
    case 0xb8: case 0xd8: case 0xf8: { // clc sec cli sei clv cld sed
      bool* const f = opcode < 0x40 ? &cpu->c : opcode < 0x80 ? &cpu->i : opcode < 0xc0 ? &cpu->v : &cpu->d;
      *f = (opcode & 0x20) && opcode != 0xb8;
      break;
    }
    case 0xa8: case 0xaa: case 0x9b: case 0xbb: case 0xba: { // tay tax txy tyx tsx
      uint16_t* const dst = (opcode == 0xa8 || opcode == 0x9b) ? &cpu->y : &cpu->x;
      const uint16_t src = opcode == 0xba ? cpu->sp : opcode == 0x9b ? cpu->x :
                           opcode == 0xbb ? cpu->y : cpu->a;
      *dst = cpu->xf ? (src & 0xff) : src;
      interp816_setZN(cpu, *dst, cpu->xf);
      break;
    }
    case 0xe8: case 0xc8: case 0xca: case 0x88: { // inx iny dex dey
      uint16_t* const r = (opcode == 0xe8 || opcode == 0xca) ? &cpu->x : &cpu->y;
      const uint16_t v = (opcode == 0xe8 || opcode == 0xc8) ? (uint16_t)(*r + 1) : (uint16_t)(*r - 1);
      *r = cpu->xf ? (v & 0xff) : v;
      interp816_setZN(cpu, *r, cpu->xf);
      break;
    }
    case 0x8a: case 0x98: case 0x1a: case 0x3a: { // txa tya inca deca
      const uint16_t v = opcode == 0x8a ? cpu->x : opcode == 0x98 ? cpu->y :
                         opcode == 0x1a ? (uint16_t)(cpu->a + 1) : (uint16_t)(cpu->a - 1);
      cpu->a = cpu->mf ? (uint16_t)((cpu->a & 0xff00) | (v & 0xff)) : v;
      interp816_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x48: case 0xda: case 0x5a: { // pha phx phy
      const uint16_t v = opcode == 0x48 ? cpu->a : opcode == 0xda ? cpu->x : cpu->y;
      if(opcode == 0x48 ? cpu->mf : cpu->xf) {
        interp816_pushByte(cpu, (uint8_t)v);
      } else {
        cpu->cyclesUsed++; // 16-bit: 1 extra cycle
        interp816_pushWord(cpu, v);
      }
      break;
    }
    case 0x68: case 0xfa: case 0x7a: { // pla plx ply
      uint16_t* const r = opcode == 0x68 ? &cpu->a : opcode == 0xfa ? &cpu->x : &cpu->y;
      const bool byte = opcode == 0x68 ? cpu->mf : cpu->xf;
      if(byte) {
        /* 8-bit A keeps its high byte; 8-bit X and Y have none */
        *r = (opcode == 0x68 ? (*r & 0xff00) : 0) | interp816_pullByte(cpu);
      } else {
        cpu->cyclesUsed++; // 16-bit: 1 extra cycle
        *r = interp816_pullWord(cpu);
      }
      interp816_setZN(cpu, *r, byte);
      break;
    }
    case 0x08: case 0x4b: case 0x8b: { // php phk phb
      interp816_pushByte(cpu, opcode == 0x08 ? interp816_getFlags(cpu) : opcode == 0x4b ? cpu->k : cpu->db);
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
          const bool byte = (op == I816_O_STX || op == I816_O_STY) ? cpu->xf : cpu->mf;
          const uint16_t v = op == I816_O_STA ? cpu->a : op == I816_O_STX ? cpu->x :
                             op == I816_O_STY ? cpu->y : 0;
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
          const bool byte = cpu->mf;
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
            case I816_O_ASL: result = value << 1; cpu->c = result & (m + 1); break;
            case I816_O_ROL: result = (value << 1) | cpu->c; cpu->c = result & (m + 1); break;
            case I816_O_LSR: cpu->c = value & 1; result = value >> 1; break;
            case I816_O_ROR: {
              const bool carry = value & 1;
              result = (value >> 1) | (cpu->c << (byte ? 7 : 15));
              if(byte) interp816_write(cpu, low, result);
              else interp816_writeWord(cpu, low, high, result, true);
              interp816_setZN(cpu, result, byte);
              cpu->c = carry;
              flags = false;
              goto rmw_done;
            }
            case I816_O_INC: result = value + 1; break;
            case I816_O_DEC: result = value - 1; break;
            case I816_O_TSB:
              cpu->z = ((cpu->a & m) & value) == 0;
              result = value | (cpu->a & m); flags = false; break;
            default: /* TRB */
              cpu->z = ((cpu->a & m) & value) == 0;
              result = value & ~(cpu->a & m); flags = false; break;
          }
          if(byte) interp816_write(cpu, low, result);
          else interp816_writeWord(cpu, low, high, result, true);
          if(flags) interp816_setZN(cpu, result, byte);
        rmw_done:
          break;
        }
        default: {  /* read an operand: AND ORA EOR CMP CPX CPY BIT LDA LDX LDY */
          const bool xop = op == I816_O_CPX || op == I816_O_CPY || op == I816_O_LDX || op == I816_O_LDY;
          const bool byte = xop ? cpu->xf : cpu->mf;
          const uint16_t m = byte ? 0xff : 0xffff;
          uint16_t value;
          if(byte) {
            value = interp816_read(cpu, low);
          } else {
            cpu->cyclesUsed++; // 16-bit: 1 extra cycle
            value = interp816_readWord(cpu, low, high);
          }
          switch (op) {
            case I816_O_AND: cpu->a = (cpu->a & ~m) | ((cpu->a & value) & m); interp816_setZN(cpu, cpu->a, byte); break;
            case I816_O_ORA: cpu->a = (cpu->a & ~m) | ((cpu->a | value) & m); interp816_setZN(cpu, cpu->a, byte); break;
            case I816_O_EOR: cpu->a = (cpu->a & ~m) | ((cpu->a ^ value) & m); interp816_setZN(cpu, cpu->a, byte); break;
            case I816_O_LDA: cpu->a = (cpu->a & ~m) | value; interp816_setZN(cpu, cpu->a, byte); break;
            case I816_O_LDX: cpu->x = value; interp816_setZN(cpu, cpu->x, byte); break;
            case I816_O_LDY: cpu->y = value; interp816_setZN(cpu, cpu->y, byte); break;
            case I816_O_BIT:
              cpu->z = ((cpu->a & m) & value) == 0;
              cpu->n = value & (byte ? 0x80 : 0x8000);
              cpu->v = value & (byte ? 0x40 : 0x4000);
              break;
            default: {  /* CMP CPX CPY */
              const uint16_t reg = op == I816_O_CPX ? cpu->x : op == I816_O_CPY ? cpu->y : cpu->a;
              const int result = (reg & m) + (value ^ m) + 1;
              cpu->c = result > m;
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
