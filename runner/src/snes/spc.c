
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "spc.h"
#include "apu.h"
#include "apu_dsp_watch.h"

static const int cyclesPerOpcode[256] = {
  2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 8,
  2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4, 6,
  2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5, 4,
  2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3, 8,
  2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6, 6,
  2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4, 3,
  2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 5, 5,
  2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 6,
  2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4, 5,
  2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12,5,
  2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4, 4,
  2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 4,
  2, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4, 9,
  2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6, 3,
  2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4, 3,
  2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4, 3
};

static inline uint8_t spc_read(Spc* spc, uint16_t adr);
static inline void spc_write(Spc* spc, uint16_t adr, uint8_t val);
static uint8_t spc_readOpcode(Spc* spc);
static uint16_t spc_readOpcodeWord(Spc* spc);
static uint8_t spc_getFlags(Spc* spc);
static void spc_setFlags(Spc* spc, uint8_t value);
static void spc_setZN(Spc* spc, uint8_t value);
static void spc_doBranch(Spc* spc, uint8_t value, bool check);
static uint8_t spc_pullByte(Spc* spc);
static void spc_pushByte(Spc* spc, uint8_t value);
static uint16_t spc_pullWord(Spc* spc);
static void spc_pushWord(Spc* spc, uint16_t value);
static uint16_t spc_readWord(Spc* spc, uint16_t adrl, uint16_t adrh);
static void spc_writeWord(Spc* spc, uint16_t adrl, uint16_t adrh, uint16_t value);
/* SNESRECOMP_COMPACT_INTERP=1 (the N64 build): the core is compiled for size
 * so its working set fits the VR4300's 16 KB I-cache (see interp816.c). */
#ifndef SNESRECOMP_COMPACT_INTERP
#define SNESRECOMP_COMPACT_INTERP 0
#endif
#if SNESRECOMP_COMPACT_INTERP && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#if defined(__GNUC__)
#define SPC_UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
#define SPC_UNREACHABLE() __assume(0)
#else
#define SPC_UNREACHABLE() ((void)0)
#endif

static void spc_doOpcode(Spc* spc, uint8_t opcode);

// addressing modes and opcode functions not declared, only used after defintions

#ifndef SNESRECOMP_TRACE
#define SNESRECOMP_TRACE 0
#endif
#ifndef SNESRECOMP_SPC_DIAGNOSTICS
#define SNESRECOMP_SPC_DIAGNOSTICS SNESRECOMP_TRACE
#endif

/* Plain ARAM accesses bypass apu_cpuRead/apu_cpuWrite. Those handle the I/O
 * page $00F0-$00FF and the IPL ROM overlay at $FFC0-$FFFF, and otherwise read
 * or write apu->ram exactly as done here. Trace and diagnostic builds keep
 * the full path for their per-access instrumentation. */
static inline uint8_t spc_read(Spc* spc, uint16_t adr) {
  if ((adr & 0xfff0) != 0x00f0 && adr < 0xffc0) {
    apu_spcReadCheck(spc->apu, adr);
    return spc->apu->ram[adr];
  }
  return apu_cpuRead(spc->apu, adr);
}

static inline void spc_write(Spc* spc, uint16_t adr, uint8_t val) {
#if !SNESRECOMP_TRACE && !SNESRECOMP_SPC_DIAGNOSTICS
  if ((adr & 0xfff0) != 0x00f0) {
    apu_spcWriteRam(spc->apu, adr, val);
    return;
  }
#endif
  apu_cpuWrite(spc->apu, adr, val);
}

Spc* spc_init(Apu* apu) {
  Spc* spc = calloc(1, sizeof(Spc));  /* zero padding: saveload/co-sim hash determinism */
  spc->apu = apu;
  return spc;
}

void spc_free(Spc* spc) {
  free(spc);
}

void spc_reset(Spc* spc) {
  spc->a = 0;
  spc->x = 0;
  spc->y = 0;
  spc->sp = 0;
  spc->pc = spc_read(spc, 0xfffe) | (spc_read(spc, 0xffff) << 8);
  spc->c = false;
  spc->z = false;
  spc->v = false;
  spc->n = false;
  spc->i = false;
  spc->h = false;
  spc->p = false;
  spc->b = false;
  spc->stopped = false;
  spc->cyclesUsed = 0;
}

void spc_saveload(Spc *spc, SaveLoadInfo *sli) {
  sli->func(sli, &spc->a, offsetof(Spc, cyclesUsed) - offsetof(Spc, a));
}

/* Opt-in SPC instruction trace (SNESRECOMP_SPC_TRACE=1): ring of the last
 * executed (pc, opcode) pairs, dumped when the core halts on SLEEP/STOP so
 * the exact path into the halt can be inspected. */
#ifndef SPC_TRACE_RING
#define SPC_TRACE_RING 65536
#endif
static uint16_t s_spc_trace_pc[SPC_TRACE_RING];
static uint8_t s_spc_trace_op[SPC_TRACE_RING];
static uint32_t s_spc_trace_head = 0;
static uint32_t s_spc_trace_count = 0;
static int s_spc_trace_on = -1;

int spc_runOpcode(Spc* spc) {
  spc->cyclesUsed = 0;
  if(spc->stopped) return 1;
  uint8_t opcode = spc_readOpcode(spc);
  if (s_spc_trace_on < 0)
    s_spc_trace_on = getenv("SNESRECOMP_SPC_TRACE") ? 1 : 0;
  if (s_spc_trace_on) {
    s_spc_trace_pc[s_spc_trace_head] = (uint16_t)(spc->pc - 1);
    s_spc_trace_op[s_spc_trace_head] = opcode;
    s_spc_trace_head = (s_spc_trace_head + 1) % SPC_TRACE_RING;
    if (s_spc_trace_count < SPC_TRACE_RING) s_spc_trace_count++;
  }
  spc->cyclesUsed = cyclesPerOpcode[opcode];
  spc_doOpcode(spc, opcode);
  if (spc->stopped && s_spc_trace_on) {
    fprintf(stderr, "[spc] HALT at pc=%04x opcode=%02x (%s) after %u traced insns\n",
            (unsigned)(spc->pc - 1), opcode,
            opcode == 0xef ? "SLEEP" : (opcode == 0xff ? "STOP" : "other"),
            s_spc_trace_count);
    uint32_t n = s_spc_trace_count < SPC_TRACE_RING ? s_spc_trace_count : SPC_TRACE_RING;
    uint32_t first = 0;  /* dump the whole ring */
    uint32_t start = (s_spc_trace_head + SPC_TRACE_RING - n) % SPC_TRACE_RING;
    for (uint32_t k = first; k < n; k++) {
      uint32_t idx = (start + k) % SPC_TRACE_RING;
      fprintf(stderr, "[spc] %04x: %02x\n", s_spc_trace_pc[idx], s_spc_trace_op[idx]);
    }
    s_spc_trace_count = 0;  /* dump once */
  }
  return spc->cyclesUsed;
}

static uint8_t spc_readOpcode(Spc* spc) {
  return spc_read(spc, spc->pc++);
}

static uint16_t spc_readOpcodeWord(Spc* spc) {
  uint8_t low = spc_readOpcode(spc);
  return low | (spc_readOpcode(spc) << 8);
}

static uint8_t spc_getFlags(Spc* spc) {
  uint8_t val = spc->n << 7;
  val |= spc->v << 6;
  val |= spc->p << 5;
  val |= spc->b << 4;
  val |= spc->h << 3;
  val |= spc->i << 2;
  val |= spc->z << 1;
  val |= spc->c;
  return val;
}

static void spc_setFlags(Spc* spc, uint8_t val) {
  spc->n = val & 0x80;
  spc->v = val & 0x40;
  spc->p = val & 0x20;
  spc->b = val & 0x10;
  spc->h = val & 8;
  spc->i = val & 4;
  spc->z = val & 2;
  spc->c = val & 1;
}

static void spc_setZN(Spc* spc, uint8_t value) {
  spc->z = value == 0;
  spc->n = value & 0x80;
}

static void spc_doBranch(Spc* spc, uint8_t value, bool check) {
  if(check) {
    spc->cyclesUsed += 2; // taken branch: 2 extra cycles
    spc->pc += (int8_t) value;
  }
}

static uint8_t spc_pullByte(Spc* spc) {
  spc->sp++;
  return spc_read(spc, 0x100 | spc->sp);
}

static void spc_pushByte(Spc* spc, uint8_t value) {
  spc_write(spc, 0x100 | spc->sp, value);
  spc->sp--;
}

static uint16_t spc_pullWord(Spc* spc) {
  uint8_t value = spc_pullByte(spc);
  return value | (spc_pullByte(spc) << 8);
}

static void spc_pushWord(Spc* spc, uint16_t value) {
  spc_pushByte(spc, value >> 8);
  spc_pushByte(spc, value & 0xff);
}

static uint16_t spc_readWord(Spc* spc, uint16_t adrl, uint16_t adrh) {
  uint8_t value = spc_read(spc, adrl);
  return value | (spc_read(spc, adrh) << 8);
}

static void spc_writeWord(Spc* spc, uint16_t adrl, uint16_t adrh, uint16_t value) {
  spc_write(spc, adrl, value & 0xff);
  spc_write(spc, adrh, value >> 8);
}

// adressing modes

static uint16_t spc_adrDp(Spc* spc) {
  return spc_readOpcode(spc) | (spc->p << 8);
}

static uint16_t spc_adrAbs(Spc* spc) {
  return spc_readOpcodeWord(spc);
}

static uint16_t spc_adrInd(Spc* spc) {
  return spc->x | (spc->p << 8);
}

static uint16_t spc_adrIdx(Spc* spc) {
  uint8_t pointer = spc_readOpcode(spc);
  return spc_readWord(spc, ((pointer + spc->x) & 0xff) | (spc->p << 8), ((pointer + spc->x + 1) & 0xff) | (spc->p << 8));
}

static uint16_t spc_adrImm(Spc* spc) {
  return spc->pc++;
}

static uint16_t spc_adrDpx(Spc* spc) {
  return ((spc_readOpcode(spc) + spc->x) & 0xff) | (spc->p << 8);
}

static uint16_t spc_adrDpy(Spc* spc) {
  return ((spc_readOpcode(spc) + spc->y) & 0xff) | (spc->p << 8);
}

static uint16_t spc_adrAbx(Spc* spc) {
  return (spc_readOpcodeWord(spc) + spc->x) & 0xffff;
}

static uint16_t spc_adrAby(Spc* spc) {
  return (spc_readOpcodeWord(spc) + spc->y) & 0xffff;
}

static uint16_t spc_adrIdy(Spc* spc) {
  uint8_t pointer = spc_readOpcode(spc);
  uint16_t adr = spc_readWord(spc, pointer | (spc->p << 8), ((pointer + 1) & 0xff) | (spc->p << 8));
  return (adr + spc->y) & 0xffff;
}

static uint16_t spc_adrDpDp(Spc* spc, uint16_t* src) {
  *src = spc_readOpcode(spc) | (spc->p << 8);
  return spc_readOpcode(spc) | (spc->p << 8);
}

static uint16_t spc_adrDpImm(Spc* spc, uint16_t* src) {
  *src = spc->pc++;
  return spc_readOpcode(spc) | (spc->p << 8);
}

static uint16_t spc_adrIndInd(Spc* spc, uint16_t* src) {
  *src = spc->y | (spc->p << 8);
  return spc->x | (spc->p << 8);
}

static uint8_t spc_adrAbsBit(Spc* spc, uint16_t* adr) {
  uint16_t adrBit = spc_readOpcodeWord(spc);
  *adr = adrBit & 0x1fff;
  return adrBit >> 13;
}

static uint16_t spc_adrDpWord(Spc* spc, uint16_t* low) {
  uint8_t adr = spc_readOpcode(spc);
  *low = adr | (spc->p << 8);
  return ((adr + 1) & 0xff) | (spc->p << 8);
}

static uint16_t spc_adrIndP(Spc* spc) {
  return spc->x++ | (spc->p << 8);
}

// opcode functions

static void spc_and(Spc* spc, uint16_t adr) {
  spc->a &= spc_read(spc, adr);
  spc_setZN(spc, spc->a);
}

static void spc_andm(Spc* spc, uint16_t dst, uint16_t src) {
  uint8_t value = spc_read(spc, src);
  uint8_t result = spc_read(spc, dst) & value;
  spc_write(spc, dst, result);
  spc_setZN(spc, result);
}

static void spc_or(Spc* spc, uint16_t adr) {
  spc->a |= spc_read(spc, adr);
  spc_setZN(spc, spc->a);
}

static void spc_orm(Spc* spc, uint16_t dst, uint16_t src) {
  uint8_t value = spc_read(spc, src);
  uint8_t result = spc_read(spc, dst) | value;
  spc_write(spc, dst, result);
  spc_setZN(spc, result);
}

static void spc_eor(Spc* spc, uint16_t adr) {
  spc->a ^= spc_read(spc, adr);
  spc_setZN(spc, spc->a);
}

static void spc_eorm(Spc* spc, uint16_t dst, uint16_t src) {
  uint8_t value = spc_read(spc, src);
  uint8_t result = spc_read(spc, dst) ^ value;
  spc_write(spc, dst, result);
  spc_setZN(spc, result);
}

static void spc_adc(Spc* spc, uint16_t adr) {
  uint8_t value = spc_read(spc, adr);
  int result = spc->a + value + spc->c;
  spc->v = (spc->a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
  spc->h = ((spc->a & 0xf) + (value & 0xf) + spc->c) > 0xf;
  spc->c = result > 0xff;
  spc->a = result;
  spc_setZN(spc, spc->a);
}

static void spc_adcm(Spc* spc, uint16_t dst, uint16_t src) {
  uint8_t value = spc_read(spc, src);
  uint8_t applyOn = spc_read(spc, dst);
  int result = applyOn + value + spc->c;
  spc->v = (applyOn & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
  spc->h = ((applyOn & 0xf) + (value & 0xf) + spc->c) > 0xf;
  spc->c = result > 0xff;
  spc_write(spc, dst, result);
  spc_setZN(spc, result);
}

static void spc_sbc(Spc* spc, uint16_t adr) {
  uint8_t value = spc_read(spc, adr) ^ 0xff;
  int result = spc->a + value + spc->c;
  spc->v = (spc->a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
  spc->h = ((spc->a & 0xf) + (value & 0xf) + spc->c) > 0xf;
  spc->c = result > 0xff;
  spc->a = result;
  spc_setZN(spc, spc->a);
}

static void spc_sbcm(Spc* spc, uint16_t dst, uint16_t src) {
  uint8_t value = spc_read(spc, src) ^ 0xff;
  uint8_t applyOn = spc_read(spc, dst);
  int result = applyOn + value + spc->c;
  spc->v = (applyOn & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
  spc->h = ((applyOn & 0xf) + (value & 0xf) + spc->c) > 0xf;
  spc->c = result > 0xff;
  spc_write(spc, dst, result);
  spc_setZN(spc, result);
}

static void spc_cmp(Spc* spc, uint16_t adr) {
  uint8_t value = spc_read(spc, adr) ^ 0xff;
  int result = spc->a + value + 1;
  spc->c = result > 0xff;
  spc_setZN(spc, result);
}

static void spc_cmpx(Spc* spc, uint16_t adr) {
  uint8_t value = spc_read(spc, adr) ^ 0xff;
  int result = spc->x + value + 1;
  spc->c = result > 0xff;
  spc_setZN(spc, result);
}

static void spc_cmpy(Spc* spc, uint16_t adr) {
  uint8_t value = spc_read(spc, adr) ^ 0xff;
  int result = spc->y + value + 1;
  spc->c = result > 0xff;
  spc_setZN(spc, result);
}

static void spc_cmpm(Spc* spc, uint16_t dst, uint16_t src) {
  uint8_t value = spc_read(spc, src) ^ 0xff;
  int result = spc_read(spc, dst) + value + 1;
  spc->c = result > 0xff;
  spc_setZN(spc, result);
}

static void spc_mov(Spc* spc, uint16_t adr) {
  spc->a = spc_read(spc, adr);
  spc_setZN(spc, spc->a);
}

static void spc_movx(Spc* spc, uint16_t adr) {
  spc->x = spc_read(spc, adr);
  spc_setZN(spc, spc->x);
}

static void spc_movy(Spc* spc, uint16_t adr) {
  spc->y = spc_read(spc, adr);
  spc_setZN(spc, spc->y);
}

static void spc_movs(Spc* spc, uint16_t adr) {
  spc_read(spc, adr);
  spc_write(spc, adr, spc->a);
}

static void spc_movsx(Spc* spc, uint16_t adr) {
  spc_read(spc, adr);
  spc_write(spc, adr, spc->x);
}

static void spc_movsy(Spc* spc, uint16_t adr) {
  spc_read(spc, adr);
  spc_write(spc, adr, spc->y);
}

static void spc_asl(Spc* spc, uint16_t adr) {
  uint8_t val = spc_read(spc, adr);
  spc->c = val & 0x80;
  val <<= 1;
  spc_write(spc, adr, val);
  spc_setZN(spc, val);
}

static void spc_lsr(Spc* spc, uint16_t adr) {
  uint8_t val = spc_read(spc, adr);
  spc->c = val & 1;
  val >>= 1;
  spc_write(spc, adr, val);
  spc_setZN(spc, val);
}

static void spc_rol(Spc* spc, uint16_t adr) {
  uint8_t val = spc_read(spc, adr);
  bool newC = val & 0x80;
  val = (val << 1) | spc->c;
  spc->c = newC;
  spc_write(spc, adr, val);
  spc_setZN(spc, val);
}

static void spc_ror(Spc* spc, uint16_t adr) {
  uint8_t val = spc_read(spc, adr);
  bool newC = val & 1;
  val = (val >> 1) | (spc->c << 7);
  spc->c = newC;
  spc_write(spc, adr, val);
  spc_setZN(spc, val);
}

static void spc_inc(Spc* spc, uint16_t adr) {
  uint8_t val = spc_read(spc, adr) + 1;
  spc_write(spc, adr, val);
  spc_setZN(spc, val);
}

static void spc_dec(Spc* spc, uint16_t adr) {
  uint8_t val = spc_read(spc, adr) - 1;
  spc_write(spc, adr, val);
  spc_setZN(spc, val);
}

/* Opcodes that are an addressing mode followed by an operation (one
 * address, or a destination and a source): their mode and operation, so
 * spc_doOpcode dispatches them through small switches instead of a case
 * each (see interp816.c). Generated from the cases they replace; the
 * helpers called, and their order, are unchanged. 0: a case of its own. */
enum { SPC_M_NONE, SPC_M_ADRDP, SPC_M_ADRABS, SPC_M_ADRIND, SPC_M_ADRIDX, SPC_M_ADRIMM, SPC_M_ADRDPX, SPC_M_ADRABX, SPC_M_ADRABY, SPC_M_ADRIDY, SPC_M_ADRDPY };
enum { SPC_O_NONE, SPC_O_OR, SPC_O_ASL, SPC_O_CMPX, SPC_O_AND, SPC_O_ROL, SPC_O_EOR, SPC_O_LSR, SPC_O_CMPY, SPC_O_CMP, SPC_O_ROR, SPC_O_ADC, SPC_O_DEC, SPC_O_MOVY, SPC_O_SBC, SPC_O_INC, SPC_O_MOVS, SPC_O_MOVSX, SPC_O_MOVSY, SPC_O_MOVX, SPC_O_MOV };
enum { SPC_M2_NONE, SPC_M2_ADRDPDP, SPC_M2_ADRDPIMM, SPC_M2_ADRINDIND };
enum { SPC_O2_NONE, SPC_O2_ORM, SPC_O2_ANDM, SPC_O2_EORM, SPC_O2_CMPM, SPC_O2_ADCM, SPC_O2_SBCM };
static const uint8_t k_spc_mode[256] = {
   0,  0,  0,  0,  1,  2,  3,  4,  5,  0,  0,  1,  2,  0,  0,  0,  /* 00 */
   0,  0,  0,  0,  6,  7,  8,  9,  0,  0,  0,  6,  0,  0,  2,  0,  /* 10 */
   0,  0,  0,  0,  1,  2,  3,  4,  5,  0,  0,  1,  2,  0,  0,  0,  /* 20 */
   0,  0,  0,  0,  6,  7,  8,  9,  0,  0,  0,  6,  0,  0,  1,  0,  /* 30 */
   0,  0,  0,  0,  1,  2,  3,  4,  5,  0,  0,  1,  2,  0,  0,  0,  /* 40 */
   0,  0,  0,  0,  6,  7,  8,  9,  0,  0,  0,  6,  0,  0,  2,  0,  /* 50 */
   0,  0,  0,  0,  1,  2,  3,  4,  5,  0,  0,  1,  2,  0,  0,  0,  /* 60 */
   0,  0,  0,  0,  6,  7,  8,  9,  0,  0,  0,  6,  0,  0,  1,  0,  /* 70 */
   0,  0,  0,  0,  1,  2,  3,  4,  5,  0,  0,  1,  2,  5,  0,  0,  /* 80 */
   0,  0,  0,  0,  6,  7,  8,  9,  0,  0,  0,  6,  0,  0,  0,  0,  /* 90 */
   0,  0,  0,  0,  1,  2,  3,  4,  5,  0,  0,  1,  2,  5,  0,  0,  /* A0 */
   0,  0,  0,  0,  6,  7,  8,  9,  0,  0,  0,  6,  0,  0,  0,  0,  /* B0 */
   0,  0,  0,  0,  1,  2,  3,  4,  5,  2,  0,  1,  2,  5,  0,  0,  /* C0 */
   0,  0,  0,  0,  6,  7,  8,  9,  1, 10,  0,  6,  0,  0,  0,  0,  /* D0 */
   0,  0,  0,  0,  1,  2,  3,  4,  5,  2,  0,  1,  2,  0,  0,  0,  /* E0 */
   0,  0,  0,  0,  6,  7,  8,  9,  1, 10,  0,  6,  0,  0,  0,  0,  /* F0 */
};
static const uint8_t k_spc_op[256] = {
   0,  0,  0,  0,  1,  1,  1,  1,  1,  0,  0,  2,  2,  0,  0,  0,  /* 00 */
   0,  0,  0,  0,  1,  1,  1,  1,  0,  0,  0,  2,  0,  0,  3,  0,  /* 10 */
   0,  0,  0,  0,  4,  4,  4,  4,  4,  0,  0,  5,  5,  0,  0,  0,  /* 20 */
   0,  0,  0,  0,  4,  4,  4,  4,  0,  0,  0,  5,  0,  0,  3,  0,  /* 30 */
   0,  0,  0,  0,  6,  6,  6,  6,  6,  0,  0,  7,  7,  0,  0,  0,  /* 40 */
   0,  0,  0,  0,  6,  6,  6,  6,  0,  0,  0,  7,  0,  0,  8,  0,  /* 50 */
   0,  0,  0,  0,  9,  9,  9,  9,  9,  0,  0, 10, 10,  0,  0,  0,  /* 60 */
   0,  0,  0,  0,  9,  9,  9,  9,  0,  0,  0, 10,  0,  0,  8,  0,  /* 70 */
   0,  0,  0,  0, 11, 11, 11, 11, 11,  0,  0, 12, 12, 13,  0,  0,  /* 80 */
   0,  0,  0,  0, 11, 11, 11, 11,  0,  0,  0, 12,  0,  0,  0,  0,  /* 90 */
   0,  0,  0,  0, 14, 14, 14, 14, 14,  0,  0, 15, 15,  8,  0,  0,  /* A0 */
   0,  0,  0,  0, 14, 14, 14, 14,  0,  0,  0, 15,  0,  0,  0,  0,  /* B0 */
   0,  0,  0,  0, 16, 16, 16, 16,  3, 17,  0, 18, 18, 19,  0,  0,  /* C0 */
   0,  0,  0,  0, 16, 16, 16, 16, 17, 17,  0, 18,  0,  0,  0,  0,  /* D0 */
   0,  0,  0,  0, 20, 20, 20, 20, 20, 19,  0, 13, 13,  0,  0,  0,  /* E0 */
   0,  0,  0,  0, 20, 20, 20, 20, 19, 19,  0, 13,  0,  0,  0,  0,  /* F0 */
};
static const uint8_t k_spc_mode2[256] = {
   0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  /* 00 */
   0,  0,  0,  0,  0,  0,  0,  0,  2,  3,  0,  0,  0,  0,  0,  0,  /* 10 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  /* 20 */
   0,  0,  0,  0,  0,  0,  0,  0,  2,  3,  0,  0,  0,  0,  0,  0,  /* 30 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  /* 40 */
   0,  0,  0,  0,  0,  0,  0,  0,  2,  3,  0,  0,  0,  0,  0,  0,  /* 50 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  /* 60 */
   0,  0,  0,  0,  0,  0,  0,  0,  2,  3,  0,  0,  0,  0,  0,  0,  /* 70 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  /* 80 */
   0,  0,  0,  0,  0,  0,  0,  0,  2,  3,  0,  0,  0,  0,  0,  0,  /* 90 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  /* A0 */
   0,  0,  0,  0,  0,  0,  0,  0,  2,  3,  0,  0,  0,  0,  0,  0,  /* B0 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* C0 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* D0 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* E0 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* F0 */
};
static const uint8_t k_spc_op2[256] = {
   0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  /* 00 */
   0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  0,  0,  0,  0,  0,  0,  /* 10 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  2,  0,  0,  0,  0,  0,  0,  /* 20 */
   0,  0,  0,  0,  0,  0,  0,  0,  2,  2,  0,  0,  0,  0,  0,  0,  /* 30 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  3,  0,  0,  0,  0,  0,  0,  /* 40 */
   0,  0,  0,  0,  0,  0,  0,  0,  3,  3,  0,  0,  0,  0,  0,  0,  /* 50 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  0,  0,  0,  0,  0,  0,  /* 60 */
   0,  0,  0,  0,  0,  0,  0,  0,  4,  4,  0,  0,  0,  0,  0,  0,  /* 70 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  5,  0,  0,  0,  0,  0,  0,  /* 80 */
   0,  0,  0,  0,  0,  0,  0,  0,  5,  5,  0,  0,  0,  0,  0,  0,  /* 90 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  6,  0,  0,  0,  0,  0,  0,  /* A0 */
   0,  0,  0,  0,  0,  0,  0,  0,  6,  6,  0,  0,  0,  0,  0,  0,  /* B0 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* C0 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* D0 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* E0 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* F0 */
};

static void spc_doOpcode(Spc* spc, uint8_t opcode) {
  switch(opcode) {
    case 0x00: { // nop imp
      // no operation
      break;
    }
    case 0x01:
    case 0x11:
    case 0x21:
    case 0x31:
    case 0x41:
    case 0x51:
    case 0x61:
    case 0x71:
    case 0x81:
    case 0x91:
    case 0xa1:
    case 0xb1:
    case 0xc1:
    case 0xd1:
    case 0xe1:
    case 0xf1: { // tcall imp
      spc_pushWord(spc, spc->pc);
      uint16_t adr = 0xffde - (2 * (opcode >> 4));
      spc->pc = spc_readWord(spc, adr, adr + 1);
      break;
    }
    case 0x02:
    case 0x22:
    case 0x42:
    case 0x62:
    case 0x82:
    case 0xa2:
    case 0xc2:
    case 0xe2: { // set1 dp
      uint16_t adr = spc_adrDp(spc);
      spc_write(spc, adr, spc_read(spc, adr) | (1 << (opcode >> 5)));
      break;
    }
    case 0x12:
    case 0x32:
    case 0x52:
    case 0x72:
    case 0x92:
    case 0xb2:
    case 0xd2:
    case 0xf2: { // clr1 dp
      uint16_t adr = spc_adrDp(spc);
      spc_write(spc, adr, spc_read(spc, adr) & ~(1 << (opcode >> 5)));
      break;
    }
    case 0x03:
    case 0x23:
    case 0x43:
    case 0x63:
    case 0x83:
    case 0xa3:
    case 0xc3:
    case 0xe3: { // bbs dp, rel
      uint8_t val = spc_read(spc, spc_adrDp(spc));
      spc_doBranch(spc, spc_readOpcode(spc), val & (1 << (opcode >> 5)));
      break;
    }
    case 0x13:
    case 0x33:
    case 0x53:
    case 0x73:
    case 0x93:
    case 0xb3:
    case 0xd3:
    case 0xf3: { // bbc dp, rel
      uint8_t val = spc_read(spc, spc_adrDp(spc));
      spc_doBranch(spc, spc_readOpcode(spc), (val & (1 << (opcode >> 5))) == 0);
      break;
    }
    case 0x0e: { // tset1 abs
      uint16_t adr = spc_adrAbs(spc);
      uint8_t val = spc_read(spc, adr);
      uint8_t result = spc->a + (val ^ 0xff) + 1;
      spc_setZN(spc, result);
      spc_write(spc, adr, val | spc->a);
      break;
    }
    case 0x0f: { // brk imp
      spc_pushWord(spc, spc->pc);
      spc_pushByte(spc, spc_getFlags(spc));
      spc->i = false;
      spc->b = true;
      spc->pc = spc_readWord(spc, 0xffde, 0xffdf);
      break;
    }
    case 0x1a: { // decw dp
      uint16_t low = 0;
      uint16_t high = spc_adrDpWord(spc, &low);
      uint16_t value = spc_readWord(spc, low, high) - 1;
      spc->z = value == 0;
      spc->n = value & 0x8000;
      spc_writeWord(spc, low, high, value);
      break;
    }
    case 0x1c: { // asla imp
      spc->c = spc->a & 0x80;
      spc->a <<= 1;
      spc_setZN(spc, spc->a);
      break;
    }
    case 0x1f: { // jmp iax
      uint16_t pointer = spc_readOpcodeWord(spc);
      spc->pc = spc_readWord(spc, (pointer + spc->x) & 0xffff, (pointer + spc->x + 1) & 0xffff);
      break;
    }
    case 0x2e: { // cbne dp, rel
      uint8_t val = spc_read(spc, spc_adrDp(spc)) ^ 0xff;
      uint8_t result = spc->a + val + 1;
      spc_doBranch(spc, spc_readOpcode(spc), result != 0);
      break;
    }
    case 0x2f: { // bra rel
      spc->pc += (int8_t) spc_readOpcode(spc);
      break;
    }
    case 0x3a: { // incw dp
      uint16_t low = 0;
      uint16_t high = spc_adrDpWord(spc, &low);
      uint16_t value = spc_readWord(spc, low, high) + 1;
      spc->z = value == 0;
      spc->n = value & 0x8000;
      spc_writeWord(spc, low, high, value);
      break;
    }
    case 0x3c: { // rola imp
      bool newC = spc->a & 0x80;
      spc->a = (spc->a << 1) | spc->c;
      spc->c = newC;
      spc_setZN(spc, spc->a);
      break;
    }
    case 0x3f: { // call abs
      uint16_t dst = spc_readOpcodeWord(spc);
      spc_pushWord(spc, spc->pc);
      spc->pc = dst;
      break;
    }
    case 0x4e: { // tclr1 abs
      uint16_t adr = spc_adrAbs(spc);
      uint8_t val = spc_read(spc, adr);
      uint8_t result = spc->a + (val ^ 0xff) + 1;
      spc_setZN(spc, result);
      spc_write(spc, adr, val & ~spc->a);
      break;
    }
    case 0x4f: { // pcall dp
      uint8_t dst = spc_readOpcode(spc);
      spc_pushWord(spc, spc->pc);
      spc->pc = 0xff00 | dst;
      break;
    }
    case 0x5a: { // cmpw dp
      uint16_t low = 0;
      uint16_t high = spc_adrDpWord(spc, &low);
      uint16_t value = spc_readWord(spc, low, high) ^ 0xffff;
      uint16_t ya = spc->a | (spc->y << 8);
      int result = ya + value + 1;
      spc->c = result > 0xffff;
      spc->z = (result & 0xffff) == 0;
      spc->n = result & 0x8000;
      break;
    }
    case 0x5c: { // lsra imp
      spc->c = spc->a & 1;
      spc->a >>= 1;
      spc_setZN(spc, spc->a);
      break;
    }
    case 0x5f: { // jmp abs
      spc->pc = spc_readOpcodeWord(spc);
      break;
    }
    case 0x6e: { // dbnz dp, rel
      uint16_t adr = spc_adrDp(spc);
      uint8_t result = spc_read(spc, adr) - 1;
      spc_write(spc, adr, result);
      spc_doBranch(spc, spc_readOpcode(spc), result != 0);
      break;
    }
    case 0x6f: { // ret imp
      spc->pc = spc_pullWord(spc);
      break;
    }
    case 0x7a: { // addw dp
      uint16_t low = 0;
      uint16_t high = spc_adrDpWord(spc, &low);
      uint16_t value = spc_readWord(spc, low, high);
      uint16_t ya = spc->a | (spc->y << 8);
      int result = ya + value;
      spc->v = (ya & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
      spc->h = ((ya & 0xfff) + (value & 0xfff) + 1) > 0xfff;
      spc->c = result > 0xffff;
      spc->z = (result & 0xffff) == 0;
      spc->n = result & 0x8000;
      spc->a = result & 0xff;
      spc->y = result >> 8;
      break;
    }
    case 0x7c: { // rora imp
      bool newC = spc->a & 1;
      spc->a = (spc->a >> 1) | (spc->c << 7);
      spc->c = newC;
      spc_setZN(spc, spc->a);
      break;
    }
    case 0x7f: { // reti imp
      spc_setFlags(spc, spc_pullByte(spc));
      spc->pc = spc_pullWord(spc);
      break;
    }
    case 0x8f: { // movm dp, imm
      uint16_t src = 0;
      uint16_t dst = spc_adrDpImm(spc, &src);
      uint8_t val = spc_read(spc, src);
      spc_read(spc, dst);
      spc_write(spc, dst, val);
      break;
    }
    case 0x9a: { // subw dp
      uint16_t low = 0;
      uint16_t high = spc_adrDpWord(spc, &low);
      uint16_t value = spc_readWord(spc, low, high) ^ 0xffff;
      uint16_t ya = spc->a | (spc->y << 8);
      int result = ya + value + 1;
      spc->v = (ya & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
      spc->h = ((ya & 0xfff) + (value & 0xfff) + 1) > 0xfff;
      spc->c = result > 0xffff;
      spc->z = (result & 0xffff) == 0;
      spc->n = result & 0x8000;
      spc->a = result & 0xff;
      spc->y = result >> 8;
      break;
    }
    case 0x9e: { // div imp
      // TODO: proper division algorithm
      uint16_t value = spc->a | (spc->y << 8);
      int result = 0xffff;
      int mod = spc->a;
      if(spc->x != 0) {
        result = value / spc->x;
        mod = value % spc->x;
      }
      spc->v = result > 0xff;
      spc->h = (spc->x & 0xf) <= (spc->y & 0xf);
      spc->a = result;
      spc->y = mod;
      spc_setZN(spc, spc->a);
      break;
    }
    case 0x9f: { // xcn imp
      spc->a = (spc->a >> 4) | (spc->a << 4);
      spc_setZN(spc, spc->a);
      break;
    }
    case 0xaf: { // movs ind+
      uint16_t adr = spc_adrIndP(spc);
      spc_write(spc, adr, spc->a);
      break;
    }
    case 0xba: { // movw dp
      uint16_t low = 0;
      uint16_t high = spc_adrDpWord(spc, &low);
      uint16_t val = spc_readWord(spc, low, high);
      spc->a = val & 0xff;
      spc->y = val >> 8;
      spc->z = val == 0;
      spc->n = val & 0x8000;
      break;
    }
    case 0xbd: { // movpx imp
      spc->sp = spc->x;
      break;
    }
    case 0xbe: { // das imp
      if(spc->a > 0x99 || !spc->c) {
        spc->a -= 0x60;
        spc->c = false;
      }
      if((spc->a & 0xf) > 9 || !spc->h) {
        spc->a -= 6;
      }
      spc_setZN(spc, spc->a);
      break;
    }
    case 0xbf: { // mov ind+
      uint16_t adr = spc_adrIndP(spc);
      spc->a = spc_read(spc, adr);
      spc_setZN(spc, spc->a);
      break;
    }
    case 0xca: { // mov1s abs.bit
      uint16_t adr = 0;
      uint8_t bit = spc_adrAbsBit(spc, &adr);
      uint8_t result = (spc_read(spc, adr) & (~(1 << bit))) | (spc->c << bit);
      spc_write(spc, adr, result);
      break;
    }
    case 0xcf: { // mul imp
      uint16_t result = spc->a * spc->y;
      spc->a = result & 0xff;
      spc->y = result >> 8;
      spc_setZN(spc, spc->y);
      break;
    }
    case 0xda: { // movws dp
      uint16_t low = 0;
      uint16_t high = spc_adrDpWord(spc, &low);
      spc_read(spc, low);
      spc_write(spc, low, spc->a);
      spc_write(spc, high, spc->y);
      break;
    }
    case 0xde: { // cbne dpx, rel
      uint8_t val = spc_read(spc, spc_adrDpx(spc)) ^ 0xff;
      uint8_t result = spc->a + val + 1;
      spc_doBranch(spc, spc_readOpcode(spc), result != 0);
      break;
    }
    case 0xdf: { // daa imp
      if(spc->a > 0x99 || spc->c) {
        spc->a += 0x60;
        spc->c = true;
      }
      if((spc->a & 0xf) > 9 || spc->h) {
        spc->a += 6;
      }
      spc_setZN(spc, spc->a);
      break;
    }
    case 0xe0: { // clrv imp
      spc->v = false;
      spc->h = false;
      break;
    }
    case 0xea: { // not1 abs.bit
      uint16_t adr = 0;
      uint8_t bit = spc_adrAbsBit(spc, &adr);
      uint8_t result = spc_read(spc, adr) ^ (1 << bit);
      spc_write(spc, adr, result);
      break;
    }
    case 0xed: { // notc imp
      spc->c = !spc->c;
      break;
    }
    case 0xef: { // sleep imp
      spc->stopped = true; // no interrupts, so sleeping stops as well
      break;
    }
    case 0xfa: { // movm dp, dp
      uint16_t src = 0;
      uint16_t dst = spc_adrDpDp(spc, &src);
      uint8_t val = spc_read(spc, src);
      spc_write(spc, dst, val);
      break;
    }
    case 0xfe: { // dbnzy rel
      spc->y--;
      spc_doBranch(spc, spc_readOpcode(spc), spc->y != 0);
      break;
    }
    case 0xff: { // stop imp
      spc->stopped = true;
      break;
    }

    /* Families of opcodes that differ only in the register or flag they use
     * (selected from the opcode): one case each, same helpers in the same
     * order as the per-opcode cases they replace. */
    case 0x10: case 0x30: case 0x50: case 0x70:
    case 0x90: case 0xb0: case 0xd0: case 0xf0: { // bpl bmi bvc bvs bcc bcs bne beq
      const uint8_t sel = opcode >> 6;   /* N, V, C, Z; taken when it equals bit 5 */
      const bool flag = sel == 0 ? spc->n : sel == 1 ? spc->v : sel == 2 ? spc->c : spc->z;
      spc_doBranch(spc, spc_readOpcode(spc), flag == ((opcode >> 5) & 1));
      break;
    }
    case 0x20: case 0x40: case 0x60: case 0x80: case 0xa0: case 0xc0: { // clrp setp clrc setc ei di
      bool* const f = opcode <= 0x40 ? &spc->p : opcode <= 0x80 ? &spc->c : &spc->i;
      *f = opcode == 0x40 || opcode == 0x80 || opcode == 0xa0;
      break;
    }
    case 0x0d: case 0x2d: case 0x4d: case 0x6d: { // pushp pusha pushx pushy
      spc_pushByte(spc, opcode == 0x0d ? spc_getFlags(spc) : opcode == 0x2d ? spc->a :
                        opcode == 0x4d ? spc->x : spc->y);
      break;
    }
    case 0x8e: case 0xae: case 0xce: case 0xee: { // popp popa popx popy
      const uint8_t v = spc_pullByte(spc);
      if(opcode == 0x8e) spc_setFlags(spc, v);
      else *(opcode == 0xae ? &spc->a : opcode == 0xce ? &spc->x : &spc->y) = v;
      break;
    }
    case 0x1d: case 0x3d: case 0x9c: case 0xbc: case 0xdc: case 0xfc: { // decx incx deca inca decy incy
      uint8_t* const r = (opcode & 0x0f) == 0x0d ? &spc->x : opcode < 0xc0 ? &spc->a : &spc->y;
      *r += (opcode == 0x3d || opcode == 0xbc || opcode == 0xfc) ? 1 : -1;
      spc_setZN(spc, *r);
      break;
    }
    case 0x5d: case 0x7d: case 0x9d: case 0xdd: case 0xfd: { // movxa movax movxp movay movya
      uint8_t* const dst = opcode == 0x5d || opcode == 0x9d ? &spc->x : opcode == 0xfd ? &spc->y : &spc->a;
      *dst = opcode == 0x5d || opcode == 0xfd ? spc->a : opcode == 0x7d ? spc->x :
             opcode == 0x9d ? spc->sp : spc->y;
      spc_setZN(spc, *dst);
      break;
    }
    case 0x0a: case 0x2a: case 0x4a: case 0x6a: case 0x8a: case 0xaa: { // or1 or1n and1 and1n eor1 mov1
      uint16_t adr = 0;
      uint8_t bit = spc_adrAbsBit(spc, &adr);
      const bool b = ((spc_read(spc, adr) >> bit) & 1) ^ (opcode == 0x2a || opcode == 0x6a);
      spc->c = opcode <= 0x2a ? (spc->c | b) : opcode <= 0x6a ? (spc->c & b) :
               opcode == 0x8a ? (spc->c ^ b) : b;
      break;
    }    default: {  /* addressing mode + operation (k_spc_mode/op, k_spc_mode2/op2) */
      if (k_spc_mode[opcode]) {
        uint16_t adr = 0;
        switch (k_spc_mode[opcode]) {
          case SPC_M_ADRDP: adr = spc_adrDp(spc); break;
          case SPC_M_ADRABS: adr = spc_adrAbs(spc); break;
          case SPC_M_ADRIND: adr = spc_adrInd(spc); break;
          case SPC_M_ADRIDX: adr = spc_adrIdx(spc); break;
          case SPC_M_ADRIMM: adr = spc_adrImm(spc); break;
          case SPC_M_ADRDPX: adr = spc_adrDpx(spc); break;
          case SPC_M_ADRABX: adr = spc_adrAbx(spc); break;
          case SPC_M_ADRABY: adr = spc_adrAby(spc); break;
          case SPC_M_ADRIDY: adr = spc_adrIdy(spc); break;
          case SPC_M_ADRDPY: adr = spc_adrDpy(spc); break;
          default: SPC_UNREACHABLE();
        }
        switch (k_spc_op[opcode]) {
          case SPC_O_OR: spc_or(spc, adr); break;
          case SPC_O_ASL: spc_asl(spc, adr); break;
          case SPC_O_CMPX: spc_cmpx(spc, adr); break;
          case SPC_O_AND: spc_and(spc, adr); break;
          case SPC_O_ROL: spc_rol(spc, adr); break;
          case SPC_O_EOR: spc_eor(spc, adr); break;
          case SPC_O_LSR: spc_lsr(spc, adr); break;
          case SPC_O_CMPY: spc_cmpy(spc, adr); break;
          case SPC_O_CMP: spc_cmp(spc, adr); break;
          case SPC_O_ROR: spc_ror(spc, adr); break;
          case SPC_O_ADC: spc_adc(spc, adr); break;
          case SPC_O_DEC: spc_dec(spc, adr); break;
          case SPC_O_MOVY: spc_movy(spc, adr); break;
          case SPC_O_SBC: spc_sbc(spc, adr); break;
          case SPC_O_INC: spc_inc(spc, adr); break;
          case SPC_O_MOVS: spc_movs(spc, adr); break;
          case SPC_O_MOVSX: spc_movsx(spc, adr); break;
          case SPC_O_MOVSY: spc_movsy(spc, adr); break;
          case SPC_O_MOVX: spc_movx(spc, adr); break;
          case SPC_O_MOV: spc_mov(spc, adr); break;
          default: SPC_UNREACHABLE();
        }
      } else {
        uint16_t src = 0, dst = 0;
        switch (k_spc_mode2[opcode]) {
          case SPC_M2_ADRDPDP: dst = spc_adrDpDp(spc, &src); break;
          case SPC_M2_ADRDPIMM: dst = spc_adrDpImm(spc, &src); break;
          case SPC_M2_ADRINDIND: dst = spc_adrIndInd(spc, &src); break;
          default: SPC_UNREACHABLE();
        }
        switch (k_spc_op2[opcode]) {
          case SPC_O2_ORM: spc_orm(spc, dst, src); break;
          case SPC_O2_ANDM: spc_andm(spc, dst, src); break;
          case SPC_O2_EORM: spc_eorm(spc, dst, src); break;
          case SPC_O2_CMPM: spc_cmpm(spc, dst, src); break;
          case SPC_O2_ADCM: spc_adcm(spc, dst, src); break;
          case SPC_O2_SBCM: spc_sbcm(spc, dst, src); break;
          default: SPC_UNREACHABLE();
        }
      }
      break;
    }
  }
}
