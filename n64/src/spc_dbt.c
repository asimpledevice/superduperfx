/* SPC700 dynamic translator, prototype (SNESRECOMP_SPC_DBT=1, n64 Makefile
 * SPCDBT=1). See docs/n64/BRINGUP_NOTES.md ("SPC700 translator").
 *
 * Straight-line SPC code up to a branch, jump, call or return (or an opcode
 * the translator does not cover) becomes one MIPS function, compiled the
 * first time the run loop (apu_runCycles_native) reaches its address and
 * kept until an SPC store hits its code or the APU resets. It must leave
 * exactly the state the interpreter (runner/src/snes/spc.c) would:
 *
 * - Registers: A, X, Y, SP, C and the inputs of Z and N live in s1-s7 for
 *   the block (Z is set when s6 is 0, N is bit 7 of s7); V and H are stored
 *   as the instructions that change them run.
 * - Memory: plain ARAM is read and written directly (s8 = ram + 0x8000).
 *   Pages the deferred S-DSP watches (reads that would sync it, writes it
 *   must log) and 32-byte lines holding translated code are checked once,
 *   when the block is entered, for every page or range the block can touch;
 *   a block that would touch one does not run (the interpreter does). The
 *   I/O page ($00F0-$00FF) is reached through the runtime (spc_dbt_io_*),
 *   with apu->cycles at the instruction's start; a computed address that
 *   lands there leaves the block before that instruction. Accesses to
 *   $F1-$F3 (control, DSP address and data, which can sync the S-DSP) end
 *   the block after their instruction.
 * - Time: the block runs only if its longest path fits before the next
 *   queued CPU port write and the end of the run. Sample edges inside it
 *   are handled at the instruction boundary where the interpreter handles
 *   them (after the instruction that starts on or spans the edge); an edge
 *   that starts or syncs an S-DSP batch ends the block there, since the
 *   page marks may have changed. The S-DSP front end is caught up before
 *   the first ARAM store after the block's start or an edge.
 *
 * Only the N64 (MIPS III, o64 ABI) is supported. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "snes/apu.h"
#include "snes/spc.h"
#include "snes/apu_dsp_watch.h"

#if SNESRECOMP_SPC_DBT

/* libdragon (n64sys.h); its header clashes with the runtime's dma_read. */
void data_cache_hit_writeback(volatile const void *, unsigned long);
void inst_cache_hit_invalidate(volatile void *, unsigned long);

#if !SNESRECOMP_APU_DEFER_DSP || SNESRECOMP_ECHO_IN_PLACE || SNESRECOMP_DSP_STREAM
#error "the SPC translator prototype covers the deferred S-DSP without echo in place only"
#endif

uint8_t g_spc_dbt_code[256];
void **g_spc_dbt_map[256];
uint32_t g_spc_dbt_stats[8];   /* blocks made, flushes, runs, cycles run, refused, no-fit, failed, bytes */
uint32_t g_spc_dbt_refused[4];   /* direct page, watched read, watched write, code */

enum { kCodeWords = 65536, kBlocks = 1536, kMaxInsns = 32, kMapPages = 48 };
static uint32_t s_code[kCodeWords] __attribute__((aligned(32)));
static uint32_t s_ncode, s_epilogue, s_exit, s_edge, s_fe, s_enter, s_link, s_dyn;
static void *s_target;   /* the block s_enter starts */
/* Bumped whenever a block's entry conditions may have changed (page marks,
 * the direct page, translated code): a block validated in the current
 * epoch may be entered without checking again, so blocks chain. */
uint32_t g_spc_dbt_epoch = 1;
/* Block ends: 0 an exit that does not chain (leaving before an instruction),
 * 1 no map page, 2 no block (or untranslatable), 3 stale epoch, 4 no time
 * left, 5 chained. 6: edges that ended a block (batch change). */
uint32_t g_spc_dbt_chainend[8];
static bool s_have_epilogue;
typedef void (*BlockFn)(void);   /* entered by jumping (s_enter, links), never called */
typedef uint32_t (*EnterFn)(Spc *spc, uint32_t c0, uint32_t edge0, uint32_t limit);
typedef struct {
  BlockFn fn;
  uint32_t epoch;                /* validated in this epoch */
  uint16_t pc, max_cycles;
  uint8_t p, npages, nw, pad;
  uint8_t page[10], pflag[10];   /* pflag: 1 read, 2 written */
  uint16_t wlo[6], whi[6];       /* byte ranges stores may touch */
  uint16_t tgt[2];               /* direct successors, translated with it */
  uint8_t ntgt;
} Block;
static Block s_blocks[kBlocks];
static uint32_t s_nblocks;
static void *s_pages[kMapPages][256];   /* map pages, handed out in order */
static uint32_t s_npages;
static const int *s_cyc;
static const uint8_t *s_mode, *s_op;

static bool s_flushed;   /* a flush happened during this run: no links or translation */
void spc_dbt_flush(void) {
  s_flushed = true;
  memset(g_spc_dbt_map, 0, sizeof g_spc_dbt_map);
  memset(g_spc_dbt_code, 0, sizeof g_spc_dbt_code);
  memset(g_spc_dbt_wait, 0, sizeof g_spc_dbt_wait);
  memset(g_spc_dbt_rpages, 0, sizeof g_spc_dbt_rpages);
  s_ncode = s_nblocks = s_npages = 0;
  s_have_epilogue = false;
  g_spc_dbt_epoch += 2;
  g_spc_dbt_stats[1]++;
}

uint32_t g_spc_dbt_exit_o;
uint8_t g_spc_dbt_rpages[32];
uint32_t g_spc_dbt_code_flushes;
void spc_dbt_code_written(uint16_t adr) {
#ifdef SPC_DBT_DUMP
  { extern void n64_platform_log(const char *fmt, ...);
    static int n;
    if (n++ < 40) n64_platform_log("DBTWRITE %04x\n", adr); }
#else
  (void)adr;
#endif
  g_spc_dbt_code_flushes++;
  spc_dbt_flush();
}

/* ---- MIPS encoding ------------------------------------------------------- */

enum { ZERO = 0, AT = 1, V0 = 2, V1 = 3, A0 = 4, A1 = 5, A2 = 6, A3 = 7,
       T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12, T5 = 13, T6 = 14, T7 = 15,
       S0 = 16, S1 = 17, S2 = 18, S3 = 19, S4 = 20, S5 = 21, S6 = 22, S7 = 23,
       T8 = 24, T9 = 25, SP = 29, S8 = 30, RA = 31 };
/* SPC state in callee-saved registers. */
enum { RSPC = S0, RA_ = S1, RX = S2, RY = S3, RSP_ = S4, RC = S5, RZ = S6, RN = S7, RRAM = S8,
       RBUD = T8, REDGE = T9 };
/* Time, for one entry from C (a run of chained blocks): t8 is the budget
 * left at the current block's start (cycles until the run's end or the
 * next queued CPU write); t9 is the budget value at which the next sample
 * edge falls. An instruction at offset o in its block starts after an edge
 * when t8 - t9 < o; the edge's cycle is F_END - t9. Both are kept in the
 * frame too (t8 and t9 do not survive calls). */
/* The frame (one per entry from C): 32 bytes of argument home, saved
 * registers, then locals. */
enum { FRAME = 176, F_END = 112, F_LIMIT = 116, F_T8 = 120, F_T9 = 124, F_FE = 128,
       F_TMP = 132, F_RET = 136, F_SAVE0 = 140, F_SAVE3 = 144, F_FLAG = 148, F_TMP2 = 152 };

static bool s_full;
static void put(uint32_t w) {
  if (s_ncode >= kCodeWords) { s_full = true; return; }
  s_code[s_ncode++] = w;
}
static void r_(int rs, int rt, int rd, int sa, int fn) {
  put((uint32_t)rs << 21 | (uint32_t)rt << 16 | (uint32_t)rd << 11 | (uint32_t)sa << 6 | (uint32_t)fn);
}
static void i_(int op, int rs, int rt, int imm) {
  put((uint32_t)op << 26 | (uint32_t)rs << 21 | (uint32_t)rt << 16 | ((uint32_t)imm & 0xffff));
}
#define ADDIU(t, s, i) i_(9, s, t, i)
#define SLTI(t, s, i) i_(10, s, t, i)
#define SLTIU(t, s, i) i_(11, s, t, i)
#define ANDI(t, s, i) i_(12, s, t, i)
#define ORI(t, s, i) i_(13, s, t, i)
#define XORI(t, s, i) i_(14, s, t, i)
#define LUI(t, i) i_(15, 0, t, i)
#define LBU(t, off, b) i_(0x24, b, t, off)
#define SB(t, off, b) i_(0x28, b, t, off)
#define SH(t, off, b) i_(0x29, b, t, off)
#define LW(t, off, b) i_(0x23, b, t, off)
#define LHU_(t, off, b) i_(0x25, b, t, off)
#define SW(t, off, b) i_(0x2b, b, t, off)
#define LD(t, off, b) i_(0x37, b, t, off)
#define SD(t, off, b) i_(0x3f, b, t, off)
#define ADDU(d, s, t) r_(s, t, d, 0, 0x21)
#define SUBU(d, s, t) r_(s, t, d, 0, 0x23)
#define AND(d, s, t) r_(s, t, d, 0, 0x24)
#define OR(d, s, t) r_(s, t, d, 0, 0x25)
#define XOR(d, s, t) r_(s, t, d, 0, 0x26)
#define NOR(d, s, t) r_(s, t, d, 0, 0x27)
#define SLTU(d, s, t) r_(s, t, d, 0, 0x2b)
#define SLT(d, s, t) r_(s, t, d, 0, 0x2a)
#define SLL(d, t, sa) r_(0, t, d, sa, 0)
#define SRL(d, t, sa) r_(0, t, d, sa, 2)
#define MOVE(d, s) ADDU(d, s, ZERO)
#define MULTU(s, t) r_(s, t, 0, 0, 0x19)
#define DIVU(s, t) r_(s, t, 0, 0, 0x1b)
#define MFLO(d) r_(0, 0, d, 0, 0x12)
#define MFHI(d) r_(0, 0, d, 0, 0x10)
#define JR(s) r_(s, 0, 0, 0, 8)
#define NOP() put(0)
static void li(int t, uint32_t v) {
  if (v < 0x10000) ORI(t, ZERO, (int)v);
  else { LUI(t, (int)(v >> 16)); if (v & 0xffff) ORI(t, t, (int)(v & 0xffff)); }
}
static void jal(void *fn) { put(3u << 26 | (((uint32_t)(uintptr_t)fn >> 2) & 0x03ffffff)); }
static void j_(uint32_t word_index) {
  put(2u << 26 | (((uint32_t)(uintptr_t)&s_code[word_index] >> 2) & 0x03ffffff));
}
/* Branches to labels, patched when the block is done. */
enum { kMaxLabels = 640, kMaxFix = 1024 };
static int32_t s_label[kMaxLabels];
static int s_nlabels;
static struct { uint32_t at; int label; } s_fix[kMaxFix];
static int s_nfix;
static int new_label(void) { s_label[s_nlabels] = -1; return s_nlabels++; }
static void bind(int l) { s_label[l] = (int32_t)s_ncode; }
static void br(int op, int rs, int rt, int l) {   /* BEQ 4, BNE 5 */
  if (s_nfix < kMaxFix) { s_fix[s_nfix].at = s_ncode; s_fix[s_nfix].label = l; s_nfix++; }
  else s_full = true;
  i_(op, rs, rt, 0);
}
#define BEQ(s, t, l) br(4, s, t, l)
#define BNE(s, t, l) br(5, s, t, l)
#define B(l) br(4, ZERO, ZERO, l)

/* ---- The shared epilogue ----------------------------------------------- */

static const int kSaved[] = { S0, S1, S2, S3, S4, S5, S6, S7, S8, RA };

static int spc_dbt_valid(void *block, Spc *spc);
static void *spc_dbt_link(Spc *spc, uint32_t *stub, uint32_t pc, uint32_t budget);
static void *spc_dbt_may_run(void *block, Spc *spc, uint32_t budget);

/* A branch to a fixed word of the buffer (the shared routines). */
static void br_to(int op, int rs, int rt, uint32_t to) {
  i_(op, rs, rt, (int)(to - (s_ncode + 1)));
}
static void jal_code(uint32_t word_index) {
  put(3u << 26 | (((uint32_t)(uintptr_t)&s_code[word_index] >> 2) & 0x03ffffff));
}

/* The routines every block shares, at the start of the buffer:
 *   epilogue  v0 = cycles run (pc already stored): stores the SPC state,
 *             returns to C
 *   exit      t0 = pc << 16 | o: end the run at the instruction at offset
 *             o of the current block (not run): the sample edges before it,
 *             then the epilogue
 *   edge      (jal) t0 = pc << 16 | o: the sample edges before offset o; if
 *             one started or synced an S-DSP batch, end the run there (the
 *             page marks may have changed); else return
 *   fe        (jal) catch the S-DSP front end up before an ARAM store
 *   enter     (called from C) a0 spc, a1 start cycle, a2 cycles to the
 *             first sample edge, a3 the budget; starts s_target
 *   link      (jal from an exit's stub, ra = the stub's word 13) t0 = pc <<
 *             16: links the stub to the block at pc (spc_dbt_link) and goes
 *             on there, or ends the run
 *   dyn       t0 = pc << 16 (a computed target, budget already charged):
 *             goes on at the block for pc if it may run, or ends the run */
static void emit_shared(void) {
  s_epilogue = s_ncode;
  SB(RA_, offsetof(Spc, a), RSPC);
  SB(RX, offsetof(Spc, x), RSPC);
  SB(RY, offsetof(Spc, y), RSPC);
  SB(RSP_, offsetof(Spc, sp), RSPC);
  SB(RC, offsetof(Spc, c), RSPC);
  SLTIU(T1, RZ, 1);
  SB(T1, offsetof(Spc, z), RSPC);
  SRL(T1, RN, 7);
  ANDI(T1, T1, 1);
  SB(T1, offsetof(Spc, n), RSPC);
  for (int i = 0; i < 10; i++) LD(kSaved[i], 32 + 8 * i, SP);
  JR(RA);
  ADDIU(SP, SP, FRAME);

  /* exit */
  s_exit = s_ncode;
  SRL(T2, T0, 16);
  SH(T2, offsetof(Spc, pc), RSPC);
  ANDI(T3, T0, 0xffff);
  SW(T3, F_TMP, SP);
  li(T1, (uint32_t)(uintptr_t)&g_spc_dbt_exit_o);
  SW(T3, 0, T1);                     /* 0: left at a block boundary */
  const uint32_t exit_loop = s_ncode;
  LW(T3, F_TMP, SP);
  SUBU(T1, RBUD, REDGE);
  SLT(T1, T1, T3);
  const uint32_t exit_test = s_ncode;
  i_(4, T1, ZERO, 0);                /* beq: edges done (patched) */
  NOP();
  LW(A1, F_END, SP);
  SUBU(A1, A1, REDGE);
  ADDIU(REDGE, REDGE, -32);
  SW(REDGE, F_T9, SP);
  LW(A0, offsetof(Spc, apu), RSPC);
  jal((void *)spc_dbt_edge);
  NOP();
  LW(RBUD, F_T8, SP);
  LW(REDGE, F_T9, SP);
  br_to(4, ZERO, ZERO, exit_loop);
  NOP();
  s_code[exit_test] |= (uint32_t)(s_ncode - (exit_test + 1)) & 0xffff;
  LW(V0, F_LIMIT, SP);
  SUBU(V0, V0, RBUD);
  LW(T3, F_TMP, SP);
  ADDU(V0, V0, T3);                  /* cycles of the whole run */
  j_(s_epilogue);
  NOP();

  /* edge */
  s_edge = s_ncode;
  SW(RA, F_RET, SP);
  SW(T0, F_TMP2, SP);
  SW(ZERO, F_FLAG, SP);
  const uint32_t edge_loop = s_ncode;
  LW(T0, F_TMP2, SP);
  ANDI(T3, T0, 0xffff);
  SUBU(T1, RBUD, REDGE);
  SLT(T1, T1, T3);
  const uint32_t edge_test = s_ncode;
  i_(4, T1, ZERO, 0);                /* beq: edges done (patched) */
  NOP();
  LW(A1, F_END, SP);
  SUBU(A1, A1, REDGE);
  ADDIU(REDGE, REDGE, -32);
  SW(REDGE, F_T9, SP);
  LW(A0, offsetof(Spc, apu), RSPC);
  jal((void *)spc_dbt_edge);
  NOP();
  LW(T1, F_FLAG, SP);
  OR(T1, T1, V0);
  SW(T1, F_FLAG, SP);
  ORI(T1, ZERO, 1);
  SW(T1, F_FE, SP);
  LW(RBUD, F_T8, SP);
  LW(REDGE, F_T9, SP);
  br_to(4, ZERO, ZERO, edge_loop);
  NOP();
  s_code[edge_test] |= (uint32_t)(s_ncode - (edge_test + 1)) & 0xffff;
  LW(T1, F_FLAG, SP);
  i_(5, T1, ZERO, 3);                /* bne: a batch changed: end the run here */
  LW(RA, F_RET, SP);
  JR(RA);
  NOP();
  LW(T0, F_TMP2, SP);
  j_(s_exit);
  NOP();

  /* fe */
  s_fe = s_ncode;
  SW(RA, F_RET, SP);
  LW(A0, offsetof(Spc, apu), RSPC);
  jal((void *)spc_dbt_fe);
  NOP();
  SW(ZERO, F_FE, SP);
  LW(RBUD, F_T8, SP);
  LW(REDGE, F_T9, SP);
  LW(RA, F_RET, SP);
  JR(RA);
  NOP();

  /* enter */
  s_enter = s_ncode;
  ADDIU(SP, SP, -FRAME);
  for (int i = 0; i < 10; i++) SD(kSaved[i], 32 + 8 * i, SP);
  MOVE(RSPC, A0);
  ADDU(T0, A1, A3);
  SW(T0, F_END, SP);
  SW(A3, F_LIMIT, SP);
  MOVE(RBUD, A3);
  SW(RBUD, F_T8, SP);
  SUBU(REDGE, A3, A2);
  SW(REDGE, F_T9, SP);
  ORI(T0, ZERO, 1);
  SW(T0, F_FE, SP);
  LBU(RA_, offsetof(Spc, a), RSPC);
  LBU(RX, offsetof(Spc, x), RSPC);
  LBU(RY, offsetof(Spc, y), RSPC);
  LBU(RSP_, offsetof(Spc, sp), RSPC);
  LBU(RC, offsetof(Spc, c), RSPC);
  LBU(T0, offsetof(Spc, z), RSPC);
  XORI(RZ, T0, 1);
  LBU(T0, offsetof(Spc, n), RSPC);
  SLL(RN, T0, 7);
  LW(T0, offsetof(Spc, apu), RSPC);
  li(T1, (uint32_t)offsetof(Apu, ram) + 0x8000u);
  ADDU(RRAM, T0, T1);
  li(T0, (uint32_t)(uintptr_t)&s_target);
  LW(T0, 0, T0);
  JR(T0);
  NOP();

  /* link */
  s_link = s_ncode;
  SW(T0, F_TMP2, SP);
  ADDIU(A1, RA, -52);                /* the stub's first word */
  SRL(A2, T0, 16);
  MOVE(A0, RSPC);
  MOVE(A3, RBUD);
  jal((void *)spc_dbt_link);
  NOP();
  LW(RBUD, F_T8, SP);
  LW(REDGE, F_T9, SP);
  i_(4, V0, ZERO, 3);                /* beq: no block to go on with: end the run */
  NOP();
  JR(V0);
  NOP();
  LW(T0, F_TMP2, SP);
  j_(s_exit);
  NOP();

  /* dyn */
  s_dyn = s_ncode;
  SW(T0, F_TMP2, SP);
  SRL(T2, T0, 16);
  SRL(T4, T2, 8);
  SLL(T4, T4, 2);
  li(T5, (uint32_t)(uintptr_t)g_spc_dbt_map);
  ADDU(T5, T5, T4);
  LW(T5, 0, T5);
  const uint32_t d0 = s_ncode;
  i_(4, T5, ZERO, 0);                /* no map page */
  NOP();
  ANDI(T4, T2, 0xff);
  SLL(T4, T4, 2);
  ADDU(T5, T5, T4);
  LW(T5, 0, T5);
  SLTIU(T6, T5, 2);
  const uint32_t d1 = s_ncode;
  i_(5, T6, ZERO, 0);                /* no block */
  NOP();
  /* Fast path: valid in this epoch and fits: straight in. */
  LW(T6, offsetof(Block, epoch), T5);
  li(T7, (uint32_t)(uintptr_t)&g_spc_dbt_epoch);
  LW(T7, 0, T7);
  const uint32_t dslow = s_ncode;
  i_(5, T6, T7, 0);                  /* bne: stale: ask C (patched) */
  LHU_(T6, offsetof(Block, max_cycles), T5);
  SLT(T6, RBUD, T6);
  const uint32_t dslow2 = s_ncode;
  i_(5, T6, ZERO, 0);                /* bne: no time: ask C, which says so (patched) */
  LW(T6, offsetof(Block, fn), T5);
  JR(T6);
  NOP();
  s_code[dslow] |= (uint32_t)(s_ncode - (dslow + 1)) & 0xffff;
  s_code[dslow2] |= (uint32_t)(s_ncode - (dslow2 + 1)) & 0xffff;
  SW(T5, F_SAVE3, SP);
  MOVE(A0, T5);
  MOVE(A1, RSPC);
  MOVE(A2, RBUD);
  jal((void *)spc_dbt_may_run);      /* valid in this epoch (checked again) and fits */
  NOP();
  LW(RBUD, F_T8, SP);
  LW(REDGE, F_T9, SP);
  const uint32_t d2 = s_ncode;
  i_(4, V0, ZERO, 0);
  NOP();
  JR(V0);
  NOP();
  const uint32_t dexit = s_ncode;
  s_code[d0] |= (uint32_t)(dexit - (d0 + 1)) & 0xffff;
  s_code[d1] |= (uint32_t)(dexit - (d1 + 1)) & 0xffff;
  s_code[d2] |= (uint32_t)(dexit - (d2 + 1)) & 0xffff;
  LW(T0, F_TMP2, SP);
  j_(s_exit);
  NOP();
}

/* ---- Translation --------------------------------------------------------- */

typedef struct {
  Block *b;
  const uint8_t *ram;
  uint8_t p;            /* the direct page: 0 or 1 */
  uint16_t pc;          /* this instruction's address */
  uint16_t o;           /* its offset in cycles from the block's start */
  int bail;             /* label: leave before this instruction */
  bool exit_after;      /* end the block after it ($F1-$F3 access) */
  bool fe_checked;      /* this instruction caught the front end up */
  bool edges_synced;    /* this instruction handled the sample edges before it */
  bool bail_used;       /* a run-time check may leave before this instruction */
  bool io;              /* it accessed the I/O page */
  bool cont;            /* a conditional branch: the block goes on at its fall-through */
  bool jumped;          /* bra/jmp followed: the block goes on at jump_to */
  uint16_t jump_to;
  uint8_t branch_len;   /* a conditional branch's length (branch_end) */
  bool io_ok;           /* computed addresses may reach the I/O page (rd/wr check) */
} Ctx;

/* Superblocks: the trace of instructions in the block (a conditional
 * branch's taken side leaves, its fall-through goes on; bra/jmp are
 * followed), cut into segments at the conditional branches. Each segment
 * after the first checks at its start that its longest path fits in the
 * time left (t8 >= its end, as an offset from the block's start): the
 * block's own max_cycles covers only the first. A branch back to an
 * instruction of the trace rebases t8 to that instruction's offset and
 * jumps there behind the same check, so driver loops stay in one block. */
static uint16_t s_tr_pc[kMaxInsns], s_tr_o[kMaxInsns];
static uint8_t s_tr_seg[kMaxInsns], s_tr_len[kMaxInsns];
static bool s_tr_io[kMaxInsns];
static int s_tr_label[kMaxInsns], s_ntr;
static int32_t s_seg_at[kMaxInsns + 1];   /* the slti to patch with the end (or -1) */
static uint16_t s_seg_end[kMaxInsns + 1];
static int s_nseg;
static struct { uint32_t at; uint8_t seg; } s_bp[kMaxInsns];
static int s_nbp;
static bool s_pages_full;
static int trace_find(uint16_t pc) {
  for (int i = 0; i < s_ntr; i++) if (s_tr_pc[i] == pc) return i;
  return -1;
}

static void add_page(Block *b, uint8_t page, uint8_t flag) {
  for (int i = 0; i < b->npages; i++)
    if (b->page[i] == page) { b->pflag[i] |= flag; return; }
  if (b->npages < 10) { b->page[b->npages] = page; b->pflag[b->npages++] = flag; }
  else s_pages_full = true;   /* too many pages: the block ends before this instruction */
}
static void add_range(Block *b, uint16_t lo, uint16_t hi, uint8_t flag) {
  add_page(b, (uint8_t)(lo >> 8), flag);
  if ((lo >> 8) != (hi >> 8)) add_page(b, (uint8_t)(hi >> 8), flag);
  if (flag & 2) {
    if (b->nw < 6) { b->wlo[b->nw] = lo; b->whi[b->nw++] = hi; }
    else s_pages_full = true;   /* the block ends before this instruction */
  }
}

static inline bool is_io(uint16_t adr) { return (adr & 0xfff0) == 0x00f0; }
static inline bool is_rom(uint16_t adr) { return adr >= 0xffc0; }

/* Call a runtime helper with apu in a0, the instruction's start cycle in
 * a3 (io_write) or a2 (io_read). Callers have set a1/a2. */
static void call_cycle_at(uint32_t o, void *fn, int cyc_reg) {
  LW(cyc_reg, F_END, SP);
  SUBU(cyc_reg, cyc_reg, RBUD);
  ADDIU(cyc_reg, cyc_reg, (int)o);    /* F_END - t8 + o: this instruction's cycle */
  LW(A0, offsetof(Spc, apu), RSPC);
  jal(fn);
  NOP();
  LW(RBUD, F_T8, SP);                 /* t8 and t9 do not survive calls */
  LW(REDGE, F_T9, SP);
  ORI(T1, ZERO, 1);                   /* the access may move the front end: */
  SW(T1, F_FE, SP);                   /* catch it up again before a store */
}
static void call_cycle(Ctx *c, void *fn, int cyc_reg) { call_cycle_at(c->o, fn, cyc_reg); }

typedef struct { int kind, label, resume, bail; uint16_t pc, o, adr; uint8_t where, src; } Stub;
static Stub s_stub[320];
static int s_nstub;

/* The sample edges before this instruction, before its first store or I/O
 * access (only those see the time): hot test inline, the work out of line;
 * a batch change ends the block before the instruction. */
static void edge_sync(Ctx *c) {
  if (c->edges_synced) return;
  c->edges_synced = true;
  const int cold = new_label(), back = new_label();
  SUBU(T1, RBUD, REDGE);
  SLTI(T1, T1, c->o);
  BNE(T1, ZERO, cold);
  NOP();
  bind(back);
  if (s_nstub < (int)(sizeof s_stub / sizeof s_stub[0])) {
    s_stub[s_nstub].kind = 1;
    s_stub[s_nstub].label = cold;
    s_stub[s_nstub].resume = back;
    s_stub[s_nstub].pc = c->pc;
    s_stub[s_nstub].o = c->o;
    s_nstub++;
  } else {
    s_full = true;
  }
}

static void fe_check(Ctx *c) {
  if (c->fe_checked) return;
  edge_sync(c);
  c->fe_checked = true;
  const int cold = new_label(), back = new_label();
  LW(T1, F_FE, SP);
  BNE(T1, ZERO, cold);
  NOP();
  bind(back);
  if (s_nstub < (int)(sizeof s_stub / sizeof s_stub[0])) {
    s_stub[s_nstub].kind = 2;
    s_stub[s_nstub].label = cold;
    s_stub[s_nstub].resume = back;
    s_nstub++;
  } else {
    s_full = true;
  }
}

/* Addresses. Static ones are known now; dynamic ones are computed into t0
 * (0-$FFFF), with a run-time check that leaves the block if they land in
 * the I/O page or the IPL ROM's range. */
typedef struct { bool dyn; uint16_t adr; bool io, any; } Addr;   /* io: t0 may be in $F0-$FF; any: anywhere */
enum { M_DP = 1, M_ABS = 2, M_IND = 3, M_IDX = 4, M_IMM = 5, M_DPX = 6, M_ABX = 7, M_ABY = 8, M_IDY = 9, M_DPY = 10 };

/* Returns false if the mode is not supported. len receives the operand
 * bytes. flag: 1 read, 2 written, 3 both. */
static inline uint16_t hi_of(uint16_t base) { return (uint16_t)(base + 255); }
static bool addr_of(Ctx *c, int mode, Addr *a, int *len, uint8_t flag) {
  const uint8_t *ram = c->ram;
  const uint16_t pc = c->pc, pg = (uint16_t)(c->p << 8);
  const uint8_t b1 = ram[(uint16_t)(pc + 1)], b2 = ram[(uint16_t)(pc + 2)];
  a->io = a->any = false;
  switch (mode) {
    case M_DP:
      a->dyn = false; a->adr = (uint16_t)(pg | b1); *len = 1;
      add_range(c->b, a->adr, a->adr, flag);
      return true;
    case M_ABS:
      a->dyn = false; a->adr = (uint16_t)(b1 | b2 << 8); *len = 2;
      if (is_rom(a->adr)) return false;
      add_range(c->b, a->adr, a->adr, flag);
      return true;
    case M_IMM:
      a->dyn = false; a->adr = (uint16_t)(pc + 1); *len = 1;
      return flag == 1;
    case M_IND: case M_DPX: case M_DPY: {
      const int idx = mode == M_DPY ? RY : RX;
      a->dyn = true; *len = mode == M_IND ? 0 : 1;
      if (!c->p && c->io_ok) edge_sync(c);   /* before t0: its helper calls clobber it */
      if (mode == M_IND) MOVE(T0, idx);
      else { ADDIU(T0, idx, b1); ANDI(T0, T0, 0xff); }
      if (!c->p && c->io_ok) {
        a->io = true;   /* rd/wr reach the I/O registers at run time */
      } else if (!c->p) {   /* page 0: the I/O registers are at $F0-$FF */
        SLTIU(T1, T0, 0xf0);
        BEQ(T1, ZERO, c->bail);
        c->bail_used = true;
        NOP();
      } else {
        ORI(T0, T0, 0x100);
      }
      add_range(c->b, pg, (uint16_t)(pg | 0xff), flag);
      return true;
    }
    case M_IDX: case M_IDY: {
      /* [dp+X] / [dp]+Y: a pointer in the direct page, then anywhere: the
       * target is checked at run time (rd/wr), not by the block's pages.
       * Edges first: rd's slow read calls C, which clobbers t0. */
      edge_sync(c);
      a->dyn = true; a->any = true; *len = 1;
      if (mode == M_IDX) {
        ADDIU(T0, RX, b1);
        ANDI(T0, T0, 0xff);
        if (!c->p) {   /* the pointer's bytes must not be I/O registers */
          SLTIU(T1, T0, 0xef);
          BEQ(T1, ZERO, c->bail);
          c->bail_used = true;
          NOP();
        } else {
          ORI(T0, T0, 0x100);
        }
        ADDU(T1, RRAM, T0);
        LBU(T2, -0x8000, T1);
        ADDIU(T1, T0, 1);
        ANDI(T1, T1, 0xff);
        if (c->p) ORI(T1, T1, 0x100);
        ADDU(T1, RRAM, T1);
        LBU(T4, -0x8000, T1);
        add_range(c->b, pg, (uint16_t)(pg | 0xff), 1);
      } else {
        const uint16_t lo = (uint16_t)(pg | b1), hi = (uint16_t)(pg | ((b1 + 1) & 0xff));
        if (is_io(lo) || is_io(hi)) return false;
        LBU(T2, (int)lo - 0x8000, RRAM);
        LBU(T4, (int)hi - 0x8000, RRAM);
        add_range(c->b, lo, lo, 1);
        add_range(c->b, hi, hi, 1);
      }
      SLL(T4, T4, 8);
      OR(T0, T2, T4);
      if (mode == M_IDY) { ADDU(T0, T0, RY); ANDI(T0, T0, 0xffff); }
      return true;
    }
    case M_ABX: case M_ABY: {
      const uint16_t base = (uint16_t)(b1 | b2 << 8);
      a->dyn = true; *len = 2;
      if (c->io_ok && (hi_of(base) < base || (base <= 0xff && hi_of(base) >= 0xf0)))
        edge_sync(c);                        /* before t0: its helper calls clobber it */
      li(T1, base);
      ADDU(T0, mode == M_ABY ? RY : RX, T1);
      ANDI(T0, T0, 0xffff);
      const uint16_t hi = (uint16_t)(base + 255);
      const bool wraps = hi < base;
      if ((wraps || (base <= 0xff && hi >= 0xf0)) && c->io_ok) {
        a->io = true;
      } else if (wraps || (base <= 0xff && hi >= 0xf0)) {   /* can reach the I/O page */
        ADDIU(T1, T0, -0xf0);
        SLTIU(T1, T1, 0x10);
        BNE(T1, ZERO, c->bail);
        c->bail_used = true;
        NOP();
      }
      if (wraps || hi >= 0xffc0) {   /* can reach the IPL ROM's range */
        SLTIU(T1, T0, 0xffc0);
        BEQ(T1, ZERO, c->bail);
        c->bail_used = true;
        NOP();
      }
      if (wraps) { add_range(c->b, base, 0xffff, flag); add_range(c->b, 0, hi, flag); }
      else add_range(c->b, base, hi, flag);
      return true;
    }
    default:
      return false;
  }
}

/* A store to plain ARAM. The page may be one the deferred S-DSP watches
 * now (it may read it: the write is logged; or the batch host's copy of it
 * is current): checked here, against Spc's marks, and then the runtime does
 * the write (spc_dbt_slow_write, out of line); otherwise a plain store.
 * where: 0 static adr, 1 the address in t0, 2 the stack ($100 + sp). */
static void st(Ctx *c, int where, uint16_t adr, int src) {
  const int cold = new_label(), back = new_label();
  if (where == 1) {
    SRL(T1, T0, 11);
    ADDU(T1, RSPC, T1);
    LBU(T2, offsetof(Spc, dspWatchLog), T1);
    LBU(T1, offsetof(Spc, dspViewFresh), T1);
    OR(T1, T1, T2);
    SRL(T2, T0, 8);
    ANDI(T2, T2, 7);
    r_(T2, T1, T1, 0, 6);          /* srlv t1, t1, t2 */
    ANDI(T1, T1, 1);
  } else {
    const uint16_t a = where == 0 ? adr : 0x100;
    LBU(T1, (int)offsetof(Spc, dspWatchLog) + (a >> 11), RSPC);
    LBU(T2, (int)offsetof(Spc, dspViewFresh) + (a >> 11), RSPC);
    OR(T1, T1, T2);
    ANDI(T1, T1, 1 << ((a >> 8) & 7));
  }
  BNE(T1, ZERO, cold);
  NOP();
  if (where == 0) SB(src, (int)adr - 0x8000, RRAM);
  else if (where == 1) { ADDU(T1, RRAM, T0); SB(src, -0x8000, T1); }
  else { ADDU(T1, RRAM, RSP_); SB(src, 0x100 - 0x8000, T1); }
  bind(back);
  if (s_nstub < (int)(sizeof s_stub / sizeof s_stub[0])) {
    Stub *x = &s_stub[s_nstub++];
    x->kind = 3; x->label = cold; x->resume = back; x->o = c->o;
    x->adr = adr; x->where = (uint8_t)where; x->src = (uint8_t)src;
  } else {
    s_full = true;
  }
}

/* Read the byte at a into dst. */
/* A computed address that may be in the I/O page: plain ARAM inline; $F4-
 * $FF through the runtime out of line (kinds 5 read, 6 write); $F0-$F3
 * (they can sync the S-DSP or end the block) leave before the instruction. */
static void io_cold(Ctx *c, int kind, int reg, bool dummy, int cold, int back) {
  c->io = true;   /* (a loop through it goes back to the run loop: see taken_exit) */
  if (s_nstub < (int)(sizeof s_stub / sizeof s_stub[0])) {
    Stub *x = &s_stub[s_nstub++];
    x->kind = kind; x->label = cold; x->resume = back; x->bail = c->bail; x->o = c->o;
    x->src = (uint8_t)reg; x->where = dummy;
    c->bail_used = true;
  } else {
    s_full = true;
  }
}
/* A target anywhere ([dp+X], [dp]+Y): I/O registers and the IPL ROM's
 * range leave before the instruction; a page the S-DSP may write (a sync
 * mark) reads through the runtime (kind 7); else a plain load. */
static void rd_any(Ctx *c, int dst) {
  ADDIU(T1, T0, -0xf0);
  SLTIU(T1, T1, 0x10);
  BNE(T1, ZERO, c->bail);
  NOP();
  SLTIU(T1, T0, 0xffc0);
  BEQ(T1, ZERO, c->bail);
  NOP();
  c->bail_used = true;
  SRL(T1, T0, 11);
  ADDU(T1, RSPC, T1);
  LBU(T1, offsetof(Spc, dspWatchSync), T1);
  SRL(T2, T0, 8);
  ANDI(T2, T2, 7);
  r_(T2, T1, T1, 0, 6);              /* srlv t1, t1, t2 */
  ANDI(T1, T1, 1);
  const int cold = new_label(), back = new_label();
  BNE(T1, ZERO, cold);
  NOP();
  ADDU(T1, RRAM, T0);
  LBU(dst, -0x8000, T1);
  bind(back);
  if (s_nstub < (int)(sizeof s_stub / sizeof s_stub[0])) {
    Stub *x = &s_stub[s_nstub++];
    x->kind = 7; x->label = cold; x->resume = back; x->o = c->o; x->src = (uint8_t)dst;
  } else {
    s_full = true;
  }
}
static void rd(Ctx *c, const Addr *a, int dst) {
  if (a->dyn && a->any) {
    rd_any(c, dst);
  } else if (a->dyn && a->io) {   /* (addr_of synced the edges before computing t0) */
    const int cold = new_label(), back = new_label();
    ADDIU(T1, T0, -0xf0);
    SLTIU(T1, T1, 0x10);
    BNE(T1, ZERO, cold);
    NOP();
    ADDU(T1, RRAM, T0);
    LBU(dst, -0x8000, T1);
    bind(back);
    io_cold(c, 5, dst, false, cold, back);
  } else if (a->dyn) {
    ADDU(T1, RRAM, T0);
    LBU(dst, -0x8000, T1);
  } else if (is_io(a->adr)) {
    edge_sync(c);
    li(A1, a->adr);
    c->io = true;
    call_cycle(c, (void *)spc_dbt_io_read, A2);
    MOVE(dst, V0);
    if (a->adr >= 0xf1 && a->adr <= 0xf3) c->exit_after = true;
  } else {
    LBU(dst, (int)a->adr - 0x8000, RRAM);
  }
}
/* Write src to a (src must not be a t-register a helper call clobbers when
 * a is in the I/O page: pass t3 or an s-register; it is moved to a2 first). */
static void wr_dummy(Ctx *c, const Addr *a, int src, bool dummy) {
  if (a->dyn && a->any) {
    /* I/O registers, and lines holding translated code (the interpreter's
     * store flushes the translations), leave before the instruction; the
     * store then goes through st's mark checks. (A store's dummy read of
     * plain memory has no effect.) */
    (void)dummy;
    ADDIU(T1, T0, -0xf0);
    SLTIU(T1, T1, 0x10);
    BNE(T1, ZERO, c->bail);
    NOP();
    SRL(T1, T0, 8);
    li(T2, (uint32_t)(uintptr_t)g_spc_dbt_code);
    ADDU(T2, T2, T1);
    LBU(T2, 0, T2);
    SRL(T1, T0, 5);
    ANDI(T1, T1, 7);
    r_(T1, T2, T2, 0, 6);            /* srlv t2, t2, t1 */
    ANDI(T2, T2, 1);
    BNE(T2, ZERO, c->bail);
    NOP();
    c->bail_used = true;
    st(c, 1, 0, src);
  } else if (a->dyn && a->io) {   /* (addr_of synced the edges before computing t0) */
    const int cold = new_label(), back = new_label();
    ADDIU(T1, T0, -0xf0);
    SLTIU(T1, T1, 0x10);
    BNE(T1, ZERO, cold);
    NOP();
    st(c, 1, 0, src);
    bind(back);
    io_cold(c, 6, src, dummy, cold, back);
  } else if (a->dyn) {
    st(c, 1, 0, src);
  } else if (is_io(a->adr)) {
    edge_sync(c);
    MOVE(A2, src);
    li(A1, a->adr);
    c->io = true;
    call_cycle(c, (void *)spc_dbt_io_write, A3);
    if (a->adr >= 0xf1 && a->adr <= 0xf3) c->exit_after = true;
  } else {
    st(c, 0, a->adr, src);
  }
}
static void wr(Ctx *c, const Addr *a, int src) { wr_dummy(c, a, src, false); }
static void set_zn(int r) { MOVE(RZ, r); MOVE(RN, r); }

/* The sample edges before the end of the block (offset `used`), out of
 * line; a batch change ends the run with pc at `pc` (not run). Then the
 * budget pays for the block. */
static void exit_edges(Ctx *c, uint16_t pc, uint32_t used, bool dyn) {
  const int cold = new_label(), back = new_label();
  SUBU(T1, RBUD, REDGE);
  SLTI(T1, T1, (int)used);
  BNE(T1, ZERO, cold);
  NOP();
  bind(back);
  if (s_nstub < (int)(sizeof s_stub / sizeof s_stub[0])) {
    Stub *x = &s_stub[s_nstub++];
    x->kind = dyn ? 4 : 1; x->label = cold; x->resume = back; x->pc = pc; x->o = (uint16_t)used;
  } else {
    s_full = true;
  }
  (void)c;
  ADDIU(RBUD, RBUD, -(int)used);
  SW(RBUD, F_T8, SP);
}

/* Ends the block going to a computed pc (in t0, 16 bits): kept as pc << 16
 * | used in F_SAVE0 for the edges' stub (a batch change ends the run there). */
static void exit_to(Ctx *c, uint32_t used) {
  SLL(T0, T0, 16);
  ORI(T0, T0, (int)used);
  SW(T0, F_SAVE0, SP);
  exit_edges(c, 0, used, true);
  LW(T0, F_SAVE0, SP);
  SRL(T0, T0, 16);
  SLL(T0, T0, 16);
  j_(s_dyn);
  NOP();
}

/* Ends the block going to pc: a link stub, 13 words, unlinked at first (a
 * branch to its last three words, which call s_link); spc_dbt_link
 * rewrites it to check the epoch and the time left and jump straight into
 * the target block (gpSP links its blocks the same way). */
enum { kStubWords = 13 };
static void exit_const(Ctx *c, uint16_t pc, uint32_t used) {
  exit_edges(c, pc, used, false);
  i_(4, ZERO, ZERO, 9);              /* b word 10 */
  NOP();
  for (int i = 0; i < 8; i++) NOP();
  LUI(T0, pc);
  jal_code(s_link);
  NOP();
}

/* A conditional branch at the end of the block: taken when t1 != 0
 * (want_set) or t1 == 0. */
static void add_tgt(Block *b, uint16_t pc) {
  if (b->ntgt < 2) b->tgt[b->ntgt++] = pc;
}
/* Leaves for target after `used` cycles: back into the trace when target is
 * one of its instructions (and the loop does no I/O: polling loops go back
 * to the run loop, whose wait skip handles them), else a link stub. */
static void taken_exit(Ctx *c, uint16_t target, uint32_t used) {
  const int i = trace_find(target);
  bool io = c->io;
  if (i >= 0) for (int k = i; k < s_ntr; k++) io |= s_tr_io[k];
#ifdef SPC_DBT_NO_BACKEDGE
  io = true;
#endif
  if (i < 0 || io || s_nbp == kMaxInsns || s_nstub >= (int)(sizeof s_stub / sizeof s_stub[0])) {
    exit_const(c, target, used);
    return;
  }
  exit_edges(c, target, used, false);
  ADDIU(RBUD, RBUD, s_tr_o[i]);      /* target's offset from the rebased start */
  SW(RBUD, F_T8, SP);
  s_bp[s_nbp].at = s_ncode;
  s_bp[s_nbp].seg = s_tr_seg[i];
  s_nbp++;
  SLTI(T1, RBUD, 0);                 /* < its segment's end (patched) */
  const int bail = new_label();
  BNE(T1, ZERO, bail);
  NOP();
  BEQ(ZERO, ZERO, s_tr_label[i]);
  NOP();
  Stub *x = &s_stub[s_nstub++];
  x->kind = 0; x->label = bail; x->pc = target; x->o = s_tr_o[i];
}
static void branch_end(Ctx *c, int len, int8_t rel, uint32_t base_cycles, bool want_set) {
  const uint16_t next = (uint16_t)(c->pc + len), target = (uint16_t)(next + rel);
  c->branch_len = (uint8_t)len;
  add_tgt(c->b, next);
  add_tgt(c->b, target);
  const int fall = new_label();
  if (want_set) BEQ(T1, ZERO, fall); else BNE(T1, ZERO, fall);
  NOP();
  taken_exit(c, target, c->o + base_cycles + 2);
  bind(fall);
  c->cont = true;                    /* the block goes on at next */
}

/* The generic opcodes (spc.c's k_spc_mode/k_spc_op): an addressing mode
 * and an operation. */
enum { O_OR = 1, O_ASL, O_CMPX, O_AND, O_ROL, O_EOR, O_LSR, O_CMPY, O_CMP, O_ROR, O_ADC, O_DEC,
       O_MOVY, O_SBC, O_INC, O_MOVS, O_MOVSX, O_MOVSY, O_MOVX, O_MOV };

static void adc_into(int acc, int val) {   /* acc (s-reg) += val + C, flags; t3 = val */
  ADDU(T4, acc, val);
  ADDU(T4, T4, RC);                  /* result */
  XOR(T5, acc, val);
  NOR(T5, T5, ZERO);
  XOR(T6, val, T4);
  AND(T5, T5, T6);
  SRL(T5, T5, 7);
  ANDI(T5, T5, 1);
  SB(T5, offsetof(Spc, v), RSPC);
  ANDI(T5, acc, 0xf);
  ANDI(T6, val, 0xf);
  ADDU(T5, T5, T6);
  ADDU(T5, T5, RC);
  SLTIU(T5, T5, 0x10);
  XORI(T5, T5, 1);
  SB(T5, offsetof(Spc, h), RSPC);
  SRL(RC, T4, 8);
  ANDI(acc, T4, 0xff);
  set_zn(acc);
}
static void cmp_with(int reg, int val) {   /* reg + (val ^ 0xff) + 1 */
  XORI(T4, val, 0xff);
  ADDU(T4, T4, reg);
  ADDIU(T4, T4, 1);
  SRL(RC, T4, 8);
  ANDI(T4, T4, 0xff);
  set_zn(T4);
}

static bool emit_generic(Ctx *c, uint8_t op, int *len) {
  const int mode = s_mode[op], o = s_op[op];
  if (!mode || !o) return false;
  const bool rmw = o == O_ASL || o == O_ROL || o == O_LSR || o == O_ROR || o == O_DEC || o == O_INC;
  const bool store = o == O_MOVS || o == O_MOVSX || o == O_MOVSY;
  const uint8_t flag = (uint8_t)(rmw || store ? 3 : 1);
  if ((rmw || store) && mode == M_IMM) return false;
  /* RMW of an I/O register: left to the interpreter. */
  if (rmw && !(mode == M_IND || mode == M_DPX || mode == M_DPY || mode == M_ABX || mode == M_ABY)) {
    const uint16_t pg = (uint16_t)(c->p << 8);
    const uint8_t b1 = c->ram[(uint16_t)(c->pc + 1)], b2 = c->ram[(uint16_t)(c->pc + 2)];
    const uint16_t adr = mode == M_DP ? (uint16_t)(pg | b1) : (uint16_t)(b1 | b2 << 8);
    if (is_io(adr) || is_rom(adr)) return false;
  }
  if (rmw || store) {
    /* A plain store needs the front end caught up first; an I/O store does
     * not, and catching up early is harmless (nothing changes ARAM between
     * here and the store). */
    fe_check(c);
  }
  Addr a;
  /* SPC_DBT_DYN_IO=1: computed addresses in $F4-$FF go through the I/O
   * helpers instead of leaving the block. Off: in SMW the hot case is the
   * CPU handshake (a polling loop through abs+X), which the run loop's
   * wait skip fast-forwards when interpreted; translated it spins (69.4 ms
   * against 65.3 ms run phase). */
#ifndef SPC_DBT_DYN_IO
#define SPC_DBT_DYN_IO 0
#endif
  c->io_ok = SPC_DBT_DYN_IO;
  const bool ok = addr_of(c, mode, &a, len, flag);
  c->io_ok = false;
  if (!ok) return false;
  switch (o) {
    case O_MOV: case O_MOVX: case O_MOVY: {
      const int r = o == O_MOV ? RA_ : o == O_MOVX ? RX : RY;
      rd(c, &a, T3);
      MOVE(r, T3);
      set_zn(r);
      return true;
    }
    case O_OR: case O_AND: case O_EOR:
      rd(c, &a, T3);
      if (o == O_OR) OR(RA_, RA_, T3); else if (o == O_AND) AND(RA_, RA_, T3); else XOR(RA_, RA_, T3);
      set_zn(RA_);
      return true;
    case O_CMP: case O_CMPX: case O_CMPY:
      rd(c, &a, T3);
      cmp_with(o == O_CMP ? RA_ : o == O_CMPX ? RX : RY, T3);
      return true;
    case O_ADC: case O_SBC:
      rd(c, &a, T3);
      if (o == O_SBC) XORI(T3, T3, 0xff);
      adc_into(RA_, T3);
      return true;
    case O_MOVS: case O_MOVSX: case O_MOVSY: {
      const int r = o == O_MOVS ? RA_ : o == O_MOVSX ? RX : RY;
      if (!a.dyn && is_io(a.adr)) rd(c, &a, T3);   /* the dummy read reaches I/O */
      wr_dummy(c, &a, r, true);                    /* (computed: in its cold path) */
      return true;
    }
    default: {   /* read-modify-write on plain memory */
      rd(c, &a, T3);
      switch (o) {
        case O_ASL: SRL(RC, T3, 7); SLL(T3, T3, 1); ANDI(T3, T3, 0xff); break;
        case O_LSR: ANDI(RC, T3, 1); SRL(T3, T3, 1); break;
        case O_ROL: SRL(T4, T3, 7); SLL(T3, T3, 1); OR(T3, T3, RC); ANDI(T3, T3, 0xff); MOVE(RC, T4); break;
        case O_ROR: ANDI(T4, T3, 1); SLL(T5, RC, 7); SRL(T3, T3, 1); OR(T3, T3, T5); MOVE(RC, T4); break;
        case O_INC: ADDIU(T3, T3, 1); ANDI(T3, T3, 0xff); break;
        case O_DEC: ADDIU(T3, T3, -1); ANDI(T3, T3, 0xff); break;
      }
      wr(c, &a, T3);
      set_zn(T3);
      return true;
    }
  }
}

/* Direct-page word operand: low and high bytes (high wraps in the page). */
static bool dp_word(Ctx *c, uint16_t *lo, uint16_t *hi, uint8_t flag) {
  const uint8_t d = c->ram[(uint16_t)(c->pc + 1)];
  *lo = (uint16_t)(c->p << 8 | d);
  *hi = (uint16_t)(c->p << 8 | ((d + 1) & 0xff));
  if (is_io(*lo) || is_io(*hi)) return false;
  add_range(c->b, *lo, *lo, flag);
  add_range(c->b, *hi, *hi, flag);
  return true;
}

/* The two-operand ALU forms on the direct page: op dp,dp ($x9) and op
 * dp,#imm ($x8), x = OR, AND, EOR, CMP, ADC, SBC (spc.c's spc_orm and
 * friends: read the source, read the destination, write it unless CMP). */
static bool emit_dp2(Ctx *c, uint8_t op, int *len) {
  if ((op & 0x0f) != 0x09 && (op & 0x0f) != 0x08) return false;
  if (op >= 0xc0 || ((op & 0x0f) == 0x08 && !(op & 0x10)) || ((op & 0x0f) == 0x09 && (op & 0x10)))
    return false;
  const unsigned k = op >> 5;   /* 0 OR, 1 AND, 2 EOR, 3 CMP, 4 ADC, 5 SBC */
  const uint8_t b1 = c->ram[(uint16_t)(c->pc + 1)], b2 = c->ram[(uint16_t)(c->pc + 2)];
  const uint16_t pg = (uint16_t)(c->p << 8);
  const bool imm = (op & 0x0f) == 0x08;
  Addr s = { false, imm ? (uint16_t)(c->pc + 1) : (uint16_t)(pg | b1), false, false };
  Addr d = { false, (uint16_t)(pg | b2), false, false };
  const bool store = k != 3;
  if (store && is_io(d.adr) && (d.adr == 0xf1 || d.adr == 0xf2 || d.adr == 0xf3 || d.adr >= 0xfd)) return false;
  if (store) fe_check(c);
  if (!imm) add_range(c->b, s.adr, s.adr, 1);
  add_range(c->b, d.adr, d.adr, store ? 3 : 1);
  rd(c, &s, T3);
  SW(T3, F_SAVE3, SP);               /* (an I/O read of d calls C) */
  rd(c, &d, T5);
  MOVE(T7, T5);
  LW(T3, F_SAVE3, SP);
  switch (k) {
    case 0: OR(T7, T7, T3); set_zn(T7); break;
    case 1: AND(T7, T7, T3); set_zn(T7); break;
    case 2: XOR(T7, T7, T3); set_zn(T7); break;
    case 3: cmp_with(T7, T3); break;
    case 4: adc_into(T7, T3); break;
    default: XORI(T3, T3, 0xff); adc_into(T7, T3); break;
  }
  if (store) wr(c, &d, T7);
  *len = 3;
  return true;
}

/* One instruction. Returns: 0 not translated, 1 translated (the block
 * goes on, or ends after it when c->exit_after is set), 2 translated and
 * ends the block (it emitted its exits). */
static int emit_insn(Ctx *c, uint8_t op, int *len) {
  const uint8_t b1 = c->ram[(uint16_t)(c->pc + 1)];
  const uint32_t cyc = (uint32_t)s_cyc[op];
  *len = 0;
  if (emit_generic(c, op, len)) { (*len)++; return 1; }
  if (emit_dp2(c, op, len)) return 1;
  *len = 1;
  switch (op) {
    case 0x5d: MOVE(RX, RA_); set_zn(RX); return 1;          /* mov x,a */
    case 0x7d: MOVE(RA_, RX); set_zn(RA_); return 1;         /* mov a,x */
    case 0xdd: MOVE(RA_, RY); set_zn(RA_); return 1;         /* mov a,y */
    case 0xfd: MOVE(RY, RA_); set_zn(RY); return 1;          /* mov y,a */
    case 0x9d: MOVE(RX, RSP_); set_zn(RX); return 1;         /* mov x,sp */
    case 0xbd: MOVE(RSP_, RX); return 1;                     /* mov sp,x */
    case 0xbc: case 0x9c: case 0x3d: case 0x1d: case 0xfc: case 0xdc: {   /* inc/dec a x y */
      const int r = (op & 0x0f) == 0x0d ? RX : op < 0xc0 ? RA_ : RY;
      ADDIU(r, r, (op == 0x3d || op == 0xbc || op == 0xfc) ? 1 : -1);
      ANDI(r, r, 0xff);
      set_zn(r);
      return 1;
    }
    case 0x1c: SRL(RC, RA_, 7); SLL(RA_, RA_, 1); ANDI(RA_, RA_, 0xff); set_zn(RA_); return 1;   /* asl a */
    case 0x5c: ANDI(RC, RA_, 1); SRL(RA_, RA_, 1); set_zn(RA_); return 1;                        /* lsr a */
    case 0x3c: SRL(T4, RA_, 7); SLL(RA_, RA_, 1); OR(RA_, RA_, RC); ANDI(RA_, RA_, 0xff);
               MOVE(RC, T4); set_zn(RA_); return 1;                                             /* rol a */
    case 0x7c: ANDI(T4, RA_, 1); SLL(T5, RC, 7); SRL(RA_, RA_, 1); OR(RA_, RA_, T5);
               MOVE(RC, T4); set_zn(RA_); return 1;                                             /* ror a */
    case 0x9f: SRL(T4, RA_, 4); SLL(T5, RA_, 4); OR(RA_, T4, T5); ANDI(RA_, RA_, 0xff);
               set_zn(RA_); return 1;                                                           /* xcn */
    case 0x60: MOVE(RC, ZERO); return 1;                     /* clrc */
    case 0x80: ORI(RC, ZERO, 1); return 1;                   /* setc */
    case 0xed: XORI(RC, RC, 1); return 1;                    /* notc */
    case 0xe0: SB(ZERO, offsetof(Spc, v), RSPC); SB(ZERO, offsetof(Spc, h), RSPC); return 1;   /* clrv */
    case 0xcf:                                               /* mul ya */
      MULTU(RA_, RY);
      MFLO(T4);
      ANDI(RA_, T4, 0xff);
      SRL(RY, T4, 8);
      ANDI(RY, RY, 0xff);
      set_zn(RY);
      return 1;
    case 0x9e: {                                             /* div ya,x */
      const int zero = new_label(), done = new_label();
      SLL(T4, RY, 8);
      OR(T4, T4, RA_);                                       /* ya */
      li(T5, 0xffff);                                        /* result */
      MOVE(T6, RA_);                                         /* mod */
      BEQ(RX, ZERO, zero);
      NOP();
      DIVU(T4, RX);
      NOP(); NOP();
      MFLO(T5);
      MFHI(T6);
      bind(zero);
      SLTIU(T7, T5, 0x100);
      XORI(T7, T7, 1);
      SB(T7, offsetof(Spc, v), RSPC);                       /* v = result > 0xff */
      ANDI(T7, RX, 0xf);
      ANDI(T4, RY, 0xf);                                     /* (t8/t9 hold the budget) */
      SLTU(T7, T4, T7);
      XORI(T7, T7, 1);
      SB(T7, offsetof(Spc, h), RSPC);                       /* h = (x & 15) <= (y & 15) */
      ANDI(RA_, T5, 0xff);
      ANDI(RY, T6, 0xff);
      set_zn(RA_);
      (void)done;
      return 1;
    }
    case 0x2d: case 0x4d: case 0x6d: {                       /* push a x y */
      fe_check(c);
      add_range(c->b, 0x100, 0x1ff, 3);
      st(c, 2, 0, op == 0x2d ? RA_ : op == 0x4d ? RX : RY);
      ADDIU(RSP_, RSP_, -1);
      ANDI(RSP_, RSP_, 0xff);
      return 1;
    }
    case 0xae: case 0xce: case 0xee: {                       /* pop a x y */
      const int r = op == 0xae ? RA_ : op == 0xce ? RX : RY;
      add_range(c->b, 0x100, 0x1ff, 1);
      ADDIU(RSP_, RSP_, 1);
      ANDI(RSP_, RSP_, 0xff);
      ADDU(T1, RRAM, RSP_);
      LBU(r, 0x100 - 0x8000, T1);
      return 1;
    }
    case 0xba: case 0x3a: case 0x1a: case 0x7a: case 0x9a: case 0x5a: {   /* word ops on dp */
      uint16_t lo, hi;
      const bool writes = op == 0x3a || op == 0x1a;
      if (!dp_word(c, &lo, &hi, writes ? 3 : 1)) return 0;
      if (writes) fe_check(c);
      LBU(T3, (int)lo - 0x8000, RRAM);
      LBU(T4, (int)hi - 0x8000, RRAM);
      SLL(T4, T4, 8);
      OR(T3, T3, T4);                                       /* value */
      *len = 2;
      if (op == 0xba) {                                     /* movw ya,dp */
        ANDI(RA_, T3, 0xff);
        SRL(RY, T3, 8);
        MOVE(RZ, T3);
        MOVE(RN, RY);
        return 1;
      }
      if (writes) {                                          /* incw / decw */
        ADDIU(T3, T3, op == 0x3a ? 1 : -1);
        ANDI(T3, T3, 0xffff);
        MOVE(RZ, T3);
        SRL(RN, T3, 8);
        st(c, 0, lo, T3);
        st(c, 0, hi, RN);
        return 1;
      }
      SLL(T7, RY, 8);
      OR(T7, T7, RA_);                                       /* ya */
      if (op != 0x7a) XORI(T3, T3, 0xffff);                 /* subw, cmpw: value ^ 0xffff */
      ADDU(T4, T7, T3);
      if (op != 0x7a) ADDIU(T4, T4, 1);                      /* result */
      SRL(RC, T4, 16);
      ANDI(T5, T4, 0xffff);
      MOVE(RZ, T5);
      SRL(RN, T5, 8);
      if (op == 0x5a) return 1;                              /* cmpw: c, z, n */
      XOR(T5, T7, T3);
      NOR(T5, T5, ZERO);
      XOR(T6, T3, T4);
      AND(T5, T5, T6);
      SRL(T5, T5, 15);
      ANDI(T5, T5, 1);
      SB(T5, offsetof(Spc, v), RSPC);
      ANDI(T5, T7, 0xfff);
      ANDI(T6, T3, 0xfff);
      ADDU(T5, T5, T6);
      ADDIU(T5, T5, 1);
      SLTIU(T5, T5, 0x1000);
      XORI(T5, T5, 1);
      SB(T5, offsetof(Spc, h), RSPC);
      ANDI(RA_, T4, 0xff);
      SRL(RY, T4, 8);
      ANDI(RY, RY, 0xff);
      return 1;
    }
    case 0xda: {                                             /* movw dp,ya */
      uint16_t lo, hi;
      if (!dp_word(c, &lo, &hi, 3)) return 0;
      fe_check(c);
      st(c, 0, lo, RA_);
      st(c, 0, hi, RY);
      *len = 2;
      return 1;
    }
    case 0x8f: case 0xfa: {                                  /* mov dp,#imm / mov dp,dp */
      const uint8_t b2 = c->ram[(uint16_t)(c->pc + 2)];
      const uint16_t dst = (uint16_t)(c->p << 8 | b2);
      Addr s = { false, op == 0x8f ? (uint16_t)(c->pc + 1) : (uint16_t)(c->p << 8 | b1) };
      Addr d = { false, dst };
      if (is_io(dst) && dst >= 0xf1 && dst <= 0xf3) { /* fine: exits after */ }
      if (!is_io(dst)) fe_check(c);
      if (op == 0xfa) add_range(c->b, s.adr, s.adr, 1);
      add_range(c->b, dst, dst, 3);
      if (op == 0x8f) LBU(T3, (int)s.adr - 0x8000, RRAM);
      else rd(c, &s, T3);
      if (op == 0x8f && is_io(dst)) {                        /* the dummy read of dst */
        SW(T3, F_TMP, SP);                                   /* t-registers do not survive calls */
        rd(c, &d, T4);
        LW(T3, F_TMP, SP);
        wr(c, &d, T3);
      } else if (is_io(dst)) {
        wr(c, &d, T3);
      } else {
        wr(c, &d, T3);
      }
      *len = 3;
      return 1;
    }
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xa2: case 0xc2: case 0xe2:
    case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xb2: case 0xd2: case 0xf2: {   /* set1/clr1 dp.b */
      const uint16_t adr = (uint16_t)(c->p << 8 | b1);
      if (is_io(adr)) return 0;
      fe_check(c);
      add_range(c->b, adr, adr, 3);
      LBU(T3, (int)adr - 0x8000, RRAM);
      if (op & 0x10) ANDI(T3, T3, ~(1 << (op >> 5)) & 0xff);
      else ORI(T3, T3, 1 << (op >> 5));
      st(c, 0, adr, T3);
      *len = 2;
      return 1;
    }
    /* ---- block ends ---- */
    case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xb0: case 0xd0: case 0xf0: {
      const unsigned sel = op >> 6;
      if (sel == 0) { SRL(T1, RN, 7); ANDI(T1, T1, 1); }
      else if (sel == 1) LBU(T1, offsetof(Spc, v), RSPC);
      else if (sel == 2) MOVE(T1, RC);
      else SLTIU(T1, RZ, 1);
      branch_end(c, 2, (int8_t)b1, cyc, (op >> 5) & 1);
      return 2;
    }
    case 0x2f: case 0x5f: {                                  /* bra, jmp !abs */
      const uint16_t t = op == 0x2f ? (uint16_t)(c->pc + 2 + (int8_t)b1)
                                    : (uint16_t)(b1 | c->ram[(uint16_t)(c->pc + 2)] << 8);
#ifdef SPC_DBT_NO_FOLLOW
      if (0) {
#else
      if (trace_find(t) < 0 && t >= 0x200 && t < 0xff00) {   /* followed */
#endif
        c->jumped = true;
        c->jump_to = t;
        *len = op == 0x2f ? 2 : 3;
        return 1;
      }
      add_tgt(c->b, t);
      taken_exit(c, t, c->o + cyc);
      return 2;
    }
    case 0xfe:                                               /* dbnz y */
      ADDIU(RY, RY, -1);
      ANDI(RY, RY, 0xff);
      MOVE(T1, RY);
      branch_end(c, 2, (int8_t)b1, cyc, true);
      return 2;
    case 0x2e: case 0xde: {                                  /* cbne dp / dp+x */
      Addr a;
      int l;
      if (op == 0x2e) {
        const uint16_t adr = (uint16_t)(c->p << 8 | b1);
        if (is_io(adr)) return 0;
      }
      if (!addr_of(c, op == 0x2e ? M_DP : M_DPX, &a, &l, 1)) return 0;
      rd(c, &a, T3);
      XOR(T1, RA_, T3);
      branch_end(c, 3, (int8_t)c->ram[(uint16_t)(c->pc + 2)], cyc, true);
      return 2;
    }
    case 0x6e: {                                             /* dbnz dp */
      const uint16_t adr = (uint16_t)(c->p << 8 | b1);
      if (is_io(adr)) return 0;
      fe_check(c);
      add_range(c->b, adr, adr, 3);
      LBU(T3, (int)adr - 0x8000, RRAM);
      ADDIU(T3, T3, -1);
      ANDI(T3, T3, 0xff);
      st(c, 0, adr, T3);
      MOVE(T1, T3);
      branch_end(c, 3, (int8_t)c->ram[(uint16_t)(c->pc + 2)], cyc, true);
      return 2;
    }
    case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xa3: case 0xc3: case 0xe3:
    case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xb3: case 0xd3: case 0xf3: {   /* bbs/bbc */
      const uint16_t adr = (uint16_t)(c->p << 8 | b1);
      if (is_io(adr)) return 0;
      add_range(c->b, adr, adr, 1);
      LBU(T3, (int)adr - 0x8000, RRAM);
      ANDI(T1, T3, 1 << (op >> 5));
      branch_end(c, 3, (int8_t)c->ram[(uint16_t)(c->pc + 2)], cyc, !(op & 0x10));
      return 2;
    }
    case 0x3f: {                                             /* call !abs */
      const uint16_t dst = (uint16_t)(b1 | c->ram[(uint16_t)(c->pc + 2)] << 8);
      const uint16_t ret = (uint16_t)(c->pc + 3);
      fe_check(c);
      add_range(c->b, 0x100, 0x1ff, 3);
      li(T3, ret >> 8);
      st(c, 2, 0, T3);
      ADDIU(RSP_, RSP_, -1);
      ANDI(RSP_, RSP_, 0xff);
      li(T3, ret & 0xff);
      st(c, 2, 0, T3);
      ADDIU(RSP_, RSP_, -1);
      ANDI(RSP_, RSP_, 0xff);
      add_tgt(c->b, dst);
      exit_const(c, dst, c->o + cyc);
      return 2;
    }
    case 0x6f:                                               /* ret */
      add_range(c->b, 0x100, 0x1ff, 1);
      ADDIU(RSP_, RSP_, 1);
      ANDI(RSP_, RSP_, 0xff);
      ADDU(T1, RRAM, RSP_);
      LBU(T0, 0x100 - 0x8000, T1);
      ADDIU(RSP_, RSP_, 1);
      ANDI(RSP_, RSP_, 0xff);
      ADDU(T1, RRAM, RSP_);
      LBU(T2, 0x100 - 0x8000, T1);
      SLL(T2, T2, 8);
      OR(T0, T0, T2);
      exit_to(c, c->o + cyc);
      return 2;
    default:
      return 0;
  }
}

static bool is_end(uint8_t op) {
  switch (op) {
    case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xb0: case 0xd0: case 0xf0:
    case 0x2f: case 0x5f: case 0xfe: case 0x2e: case 0xde: case 0x6e: case 0x3f: case 0x6f:
      return true;
    default:
      return (op & 0x0f) == 0x03;   /* bbs/bbc */
  }
}

static void **map_page(uint8_t page) {
  if (!g_spc_dbt_map[page]) {
    if (s_npages == kMapPages) return NULL;
    g_spc_dbt_map[page] = s_pages[s_npages++];
    memset(g_spc_dbt_map[page], 0, sizeof s_pages[0]);
  }
  return g_spc_dbt_map[page];
}

/* Translate the block at pc for direct page p; NULL if nothing could be. */
static Block *translate(Apu *apu, uint16_t pc, uint8_t p, bool may_flush) {
  if (!s_cyc) spc_tables(&s_cyc, &s_mode, &s_op);
  if (pc < 0x200 || pc >= 0xff00) return NULL;
  if (s_nblocks == kBlocks || s_ncode + 2048 > kCodeWords) {
    if (!may_flush) return NULL;   /* translated code is running: no flush */
#ifdef SPC_DBT_DUMP
    { extern void n64_platform_log(const char *fmt, ...);
      static int n;
      if (n++ < 10) n64_platform_log("DBTFULL blocks %lu words %lu\n", (unsigned long)s_nblocks, (unsigned long)s_ncode); }
#endif
    spc_dbt_flush();
  }
  if (!s_have_epilogue) {
    s_full = false;
    emit_shared();
    s_have_epilogue = true;
    /* Out of the D-cache and into the I-cache, as for every block. */
    data_cache_hit_writeback(&s_code[0], s_ncode * 4);
    inst_cache_hit_invalidate(&s_code[0], s_ncode * 4);
#ifdef N64_PROFILE_START
    { /* The profiler names samples in these (n64/profile/symbolize.py). */
      extern void n64_platform_log(const char *fmt, ...);
      static bool logged;
      if (!logged) {
        logged = true;
        const struct { uint32_t at; const char *name; } r[] = {
          { s_epilogue, "dbt_epilogue" }, { s_exit, "dbt_exit" }, { s_edge, "dbt_edge" }, { s_fe, "dbt_fe" },
          { s_enter, "dbt_enter" }, { s_link, "dbt_link" }, { s_dyn, "dbt_dyn" }, { s_ncode, "dbt_blocks" } };
        for (int i = 0; i < 8; i++) {
          const uint32_t end = i < 7 ? r[i + 1].at : kCodeWords;
          n64_platform_log("PROFMOD %08lx %lu %s\n", (unsigned long)(uintptr_t)&s_code[r[i].at],
                           (unsigned long)((end - r[i].at) * 4), r[i].name);
        }
      }
    }
#endif
  }
  const uint32_t start = s_ncode;
  Block *b = &s_blocks[s_nblocks];
  memset(b, 0, sizeof *b);
  b->pc = pc;
  b->p = p;
  s_full = false;
  s_nlabels = s_nfix = 0;
  Ctx c = { b, apu->ram, p, pc, 0, 0, false, false };
  s_nstub = 0;

  int n = 0, ended = 0;
  uint32_t max = 0;
  uint16_t end_pc = pc;
  (void)end_pc;   /* (the dump below) */
  s_ntr = 0;
  s_nseg = 1;
  s_seg_at[0] = -1;
  s_nbp = 0;
  bool open_seg = false;   /* a segment starts at this instruction: check its time */

  while (n < kMaxInsns) {
    const uint8_t op = c.ram[c.pc];
    if (c.pc >= 0xff00 || c.pc < 0x200) break;
    c.bail = new_label();
    c.exit_after = false;
    c.fe_checked = false;
    c.edges_synced = false;
    c.bail_used = false;
    c.io = c.cont = c.jumped = false;
    s_pages_full = false;
    const uint32_t ncode0 = s_ncode;
    const int nfix0 = s_nfix;
    const uint8_t npages0 = b->npages, nw0 = b->nw;
    const int nstub0 = s_nstub, nbp0 = s_nbp;
    const int tri = s_ntr;
    s_tr_pc[tri] = c.pc;
    s_tr_o[tri] = c.o;
    s_tr_seg[tri] = (uint8_t)(s_nseg - 1);
    s_tr_io[tri] = false;
    s_tr_label[tri] = new_label();
    bind(s_tr_label[tri]);
    s_ntr++;
    if (open_seg) {               /* the segment's longest path must fit */
      s_seg_at[s_nseg - 1] = (int32_t)s_ncode;
      SLTI(T1, RBUD, 0);
      BNE(T1, ZERO, c.bail);
      NOP();
      c.bail_used = true;
    }
    int len = 0;
    c.branch_len = 0;
    int r = emit_insn(&c, op, &len);
    if (c.branch_len) len = c.branch_len;   /* (the branch cases leave len at 1) */
    /* The instruction's code bytes are read from ARAM. */
    if (r) add_range(b, c.pc, (uint16_t)(c.pc + len - 1), 1);
    if (!r && n) {                /* no block can start here: the run loop starts one after it */
      void **const pg = map_page((uint8_t)(c.pc >> 8));
      if (pg && !pg[c.pc & 255]) pg[c.pc & 255] = (void *)1;
    }
    if (!r || s_pages_full) {     /* not covered (or one page too many): drop it */
      s_ncode = ncode0;
      s_nfix = nfix0;
      b->npages = npages0;
      b->nw = nw0;
      s_nstub = nstub0;
      s_nbp = nbp0;
      s_ntr = tri;
      if (open_seg) s_seg_at[s_nseg - 1] = -1;
      s_pages_full = false;
      break;
    }
    s_tr_io[tri] = c.io;
    s_tr_len[tri] = (uint8_t)len;
    const uint16_t next = c.jumped ? c.jump_to : (uint16_t)(c.pc + len);
    const uint32_t o_next = c.o + (uint32_t)s_cyc[op];
    max = o_next + (is_end(op) && op != 0x2f && op != 0x5f && op != 0x3f && op != 0x6f ? 2u : 0u);
    if (c.bail_used) {
      s_stub[s_nstub].kind = 0;   /* leaving before this instruction */
      s_stub[s_nstub].label = c.bail;
      s_stub[s_nstub].pc = c.pc;
      s_stub[s_nstub].o = c.o;
      s_nstub++;
    }
    n++;
    end_pc = (uint16_t)(c.pc + len);
    open_seg = false;
    if (c.cont) {                 /* a conditional branch: the fall-through goes on */
      s_seg_end[s_nseg - 1] = (uint16_t)max;
      s_seg_at[s_nseg] = -1;
      s_nseg++;
      open_seg = true;
      c.pc = next;
      c.o = (uint16_t)o_next;
      continue;
    }
    if (c.jumped) { c.pc = next; c.o = (uint16_t)o_next; continue; }
    if (r == 2 || is_end(op)) { ended = 1; break; }
    c.pc = next;
    c.o = (uint16_t)o_next;
    if (c.exit_after) { exit_const(&c, next, o_next); ended = 1; break; }
  }
  if (!n || s_full) {
    s_ncode = start;
    return NULL;
  }
  if (!ended) { add_tgt(b, c.pc); exit_const(&c, c.pc, c.o); }
  /* The longest path of each segment: the block never starts unless its
   * first fits in the time left; each later one checks at its start. */
  s_seg_end[s_nseg - 1] = (uint16_t)(ended ? max : c.o);
  b->max_cycles = s_seg_end[0];
  for (int i = 1; i < s_nseg; i++)
    if (s_seg_at[i] >= 0) s_code[s_seg_at[i]] |= s_seg_end[i];
  for (int i = 0; i < s_nbp; i++) s_code[s_bp[i].at] |= s_seg_end[s_bp[i].seg];
  /* The cold paths. */
  for (int i = 0; i < s_nstub; i++) {
    const Stub *st = &s_stub[i];
    bind(st->label);
    if (st->kind == 0) {          /* leave before the instruction */
      li(T0, (uint32_t)st->pc << 16 | st->o);
      j_(s_exit);
      NOP();
    } else if (st->kind == 1 || st->kind == 4) {   /* the sample edges before a point */
      if (st->kind == 1) li(T0, (uint32_t)st->pc << 16 | st->o);
      else LW(T0, F_SAVE0, SP);
      jal_code(s_edge);
      NOP();
      B(st->resume);
      NOP();
    } else if (st->kind == 3) {   /* a store to a watched page: the runtime's write */
      SW(T0, F_SAVE0, SP);
      SW(T3, F_SAVE3, SP);
      if (st->where == 0) li(A1, st->adr);
      else if (st->where == 1) MOVE(A1, T0);
      else ADDIU(A1, RSP_, 0x100);
      MOVE(A2, st->src);
      LW(A3, F_END, SP);
      SUBU(A3, A3, RBUD);
      ADDIU(A3, A3, st->o);
      LW(A0, offsetof(Spc, apu), RSPC);
      jal((void *)spc_dbt_slow_write);
      NOP();
      LW(RBUD, F_T8, SP);
      LW(REDGE, F_T9, SP);
      ORI(T1, ZERO, 1);
      SW(T1, F_FE, SP);
      LW(T0, F_SAVE0, SP);
      LW(T3, F_SAVE3, SP);
      B(st->resume);
      NOP();
    } else if (st->kind == 7) {   /* a read of a page the S-DSP may write */
      SW(T0, F_SAVE0, SP);
      MOVE(A1, T0);
      call_cycle_at(st->o, (void *)spc_dbt_read_any, A2);
      MOVE(st->src, V0);
      LW(T0, F_SAVE0, SP);
      B(st->resume);
      NOP();
    } else if (st->kind == 5 || st->kind == 6) {   /* a computed address in $F0-$FF */
      ADDIU(T1, T0, -0xf4);
      SLTIU(T1, T1, 12);
      BEQ(T1, ZERO, st->bail);    /* $F0-$F3: the interpreter runs it */
      NOP();
      SW(T0, F_SAVE0, SP);
      if (st->kind == 6) SW(st->src, F_SAVE3, SP);
      if (st->kind == 5 || st->where) {   /* the read (or a store's dummy read) */
        MOVE(A1, T0);
        call_cycle_at(st->o, (void *)spc_dbt_io_read, A2);
        if (st->kind == 5) MOVE(st->src, V0);
        LW(T0, F_SAVE0, SP);
      }
      if (st->kind == 6) {
        LW(A2, F_SAVE3, SP);
        MOVE(A1, T0);
        call_cycle_at(st->o, (void *)spc_dbt_io_write, A3);
        LW(T0, F_SAVE0, SP);
      }
      B(st->resume);
      NOP();
    } else {                      /* the front end before a store */
      jal_code(s_fe);
      NOP();
      B(st->resume);
      NOP();
    }
  }
  /* Patch the branches. */
  for (int i = 0; i < s_nfix; i++) {
    const int32_t to = s_label[s_fix[i].label];
    if (to < 0) { s_ncode = start; return NULL; }
    const int32_t off = to - (int32_t)(s_fix[i].at + 1);
    s_code[s_fix[i].at] |= (uint32_t)off & 0xffff;
  }
  if (s_full) { s_ncode = start; return NULL; }
  data_cache_hit_writeback(&s_code[start], (s_ncode - start) * 4);
  inst_cache_hit_invalidate(&s_code[start], (s_ncode - start) * 4);
  b->fn = (BlockFn)(void *)&s_code[start];
#ifdef SPC_DBT_DUMP
  { /* Debug: the first blocks' code, for an offline disassembly. */
    extern void n64_platform_log(const char *fmt, ...);
    static int dumped;
#ifndef SPC_DBT_DUMP_BLOCKS
#define SPC_DBT_DUMP_BLOCKS 0
#endif
    if (dumped++ < SPC_DBT_DUMP_BLOCKS) {
      n64_platform_log("DBTBLOCK pc %04x at %08lx words %lu epi %08lx spc", pc, (unsigned long)(uintptr_t)&s_code[start],
                       (unsigned long)(s_ncode - start), (unsigned long)(uintptr_t)&s_code[s_epilogue]);
      for (uint16_t a = pc; a != end_pc; a++) n64_platform_log(" %02x", c.ram[a]);
      n64_platform_log("\n");
      for (uint32_t i = start; i < s_ncode; i++) n64_platform_log("DBTW %08lx\n", (unsigned long)s_code[i]);
    }
  }
#endif
  /* Its read pages: a new sync mark on one starts an epoch (apu_dspMark). */
  for (int i = 0; i < b->npages; i++)
    if (b->pflag[i] & 1) g_spc_dbt_rpages[b->page[i] >> 3] |= (uint8_t)(1u << (b->page[i] & 7));
  /* Mark its code lines, so stores to them flush. */
  uint8_t new_lines = 0;
  for (int i = 0; i < s_ntr; i++)
    for (uint16_t a = s_tr_pc[i], k = 0; k < s_tr_len[i]; a++, k++) {
      const uint8_t bit = (uint8_t)(1u << ((a >> 5) & 7));
      new_lines |= bit & ~g_spc_dbt_code[a >> 8];
      g_spc_dbt_code[a >> 8] |= bit;
    }
  s_nblocks++;
  /* New code lines: blocks validated before may store to them. (Lines
   * already marked were part of every validation since.) */
  if (new_lines) g_spc_dbt_epoch += 2;
  g_spc_dbt_stats[0]++;
  g_spc_dbt_stats[7] += (s_ncode - start) * 4;
  return b;
}

static bool code_in(uint16_t lo, uint16_t hi) {
  for (uint32_t l = lo >> 5; l <= (uint32_t)(hi >> 5); l++)
    if (g_spc_dbt_code[l >> 3] & (1u << (l & 7))) return true;
  return false;
}

/* Whether block b may run with the current page marks and direct page;
 * records the epoch it was checked in. Also called by the shared chain
 * code for a stale successor. */
static int spc_dbt_valid(void *block, Spc *spc) {
  Block *const b = (Block *)block;
  if (spc->p != b->p) { g_spc_dbt_stats[4]++; g_spc_dbt_refused[0]++; return 0; }
  for (int i = 0; i < b->npages; i++) {
    const uint16_t adr = (uint16_t)(b->page[i] << 8);
    if ((b->pflag[i] & 1) && APU_DSP_PAGE_BIT(spc->dspWatchSync, adr)) {
      g_spc_dbt_stats[4]++; g_spc_dbt_refused[1]++;
      return 0;
    }
  }
  for (int i = 0; i < b->nw; i++)
    if (code_in(b->wlo[i], b->whi[i])) { g_spc_dbt_stats[4]++; g_spc_dbt_refused[3]++; return 0; }
  /* (A wait loop's block stays stale, so links into it ask C each time.) */
  if (!(g_spc_dbt_wait[b->pc >> 3] & (1u << (b->pc & 7)))) b->epoch = g_spc_dbt_epoch;
  return 1;
}

/* Translate at pc and record the result (or that there is none) in the map. */
static Block *translate_at(Apu *apu, uint16_t pc, uint8_t p, bool may_flush) {
  Block *b = translate(apu, pc, p, may_flush);
  void **pg = map_page((uint8_t)(pc >> 8));   /* a flush may have cleared the map */
  if (!pg) return NULL;
  pg[pc & 255] = b ? (void *)b : (void *)1;
  return b;
}

/* Whether block b may start now with `budget` cycles left: valid in this
 * epoch (checked again if not) and short enough. Returns its code or NULL. */
static void *spc_dbt_may_run(void *block, Spc *spc, uint32_t budget) {
  Block *const b = (Block *)block;
  if (s_flushed) { g_spc_dbt_chainend[1]++; return NULL; }
  if (g_spc_dbt_wait[b->pc >> 3] & (1u << (b->pc & 7))) { g_spc_dbt_chainend[4]++; return NULL; }   /* wait loop */
  if (b->epoch != g_spc_dbt_epoch && !spc_dbt_valid(b, spc)) { g_spc_dbt_chainend[2]++; return NULL; }
  if (b->max_cycles > budget) { g_spc_dbt_chainend[3]++; return NULL; }
  g_spc_dbt_chainend[5]++;
  return (void *)b->fn;
}

/* An exit's stub reached its call to s_link: find (or translate, without a
 * flush) the block at pc, rewrite the stub to go straight to it, and say
 * whether to go on there now. */
static void *spc_dbt_link(Spc *spc, uint32_t *stub, uint32_t pc, uint32_t budget) {
  if (s_flushed) { g_spc_dbt_chainend[1]++; return NULL; }
  void **pg = map_page((uint8_t)(pc >> 8));
  if (!pg) { g_spc_dbt_chainend[1]++; return NULL; }
  Block *b = (Block *)pg[pc & 255];
  if (!b) b = translate_at(spc->apu, (uint16_t)pc, spc->p, false);
  if (!b || (void *)b == (void *)1) { g_spc_dbt_chainend[1]++; return NULL; }
  /* The linked stub: epoch check, time check, jump; its words 10-12 stay
   * the call to s_link (taken when either check fails). */
  const uint32_t ea = (uint32_t)(uintptr_t)&b->epoch, ga = (uint32_t)(uintptr_t)&g_spc_dbt_epoch;
  const uint32_t w[10] = {
    15u << 26 | (uint32_t)T2 << 16 | (((ea + 0x8000u) >> 16) & 0xffff),            /* lui t2, %hi(&b->epoch) */
    0x23u << 26 | (uint32_t)T2 << 21 | (uint32_t)T2 << 16 | (ea & 0xffff),         /* lw t2, %lo(t2) */
    15u << 26 | (uint32_t)T3 << 16 | (((ga + 0x8000u) >> 16) & 0xffff),            /* lui t3, %hi(&epoch) */
    0x23u << 26 | (uint32_t)T3 << 21 | (uint32_t)T3 << 16 | (ga & 0xffff),         /* lw t3, %lo(t3) */
    5u << 26 | (uint32_t)T2 << 21 | (uint32_t)T3 << 16 | 5u,                        /* bne t2, t3, word 10 */
    10u << 26 | (uint32_t)RBUD << 21 | (uint32_t)T1 << 16 | b->max_cycles,          /* slti t1, t8, max */
    5u << 26 | (uint32_t)T1 << 21 | 3u,                                             /* bne t1, zero, word 10 */
    0,
    2u << 26 | (((uint32_t)(uintptr_t)b->fn >> 2) & 0x03ffffff),                   /* j the block */
    0,
  };
  if (stub[8] != w[8] || stub[5] != w[5]) {
    for (int i = 0; i < 10; i++) stub[i] = w[i];
    data_cache_hit_writeback(stub, 40);
    inst_cache_hit_invalidate(stub, 40);
    g_spc_dbt_chainend[0]++;
  }
  return spc_dbt_may_run(b, spc, budget);
}

uint32_t spc_dbt_run(Apu *apu, uint32_t limit) {
  Spc *const spc = apu->spc;
  const uint16_t pc = spc->pc;
  void **pg = map_page((uint8_t)(pc >> 8));
  if (!pg) return 0;
  Block *b = (Block *)pg[pc & 255];
  if (!b) {
    const uint32_t flushes = g_spc_dbt_stats[1];
    b = translate_at(apu, pc, spc->p, true);
    if (!b) { g_spc_dbt_stats[6]++; return 0; }
    (void)flushes;
  }
  if ((void *)b == (void *)1) return 0;
  if (b->max_cycles > limit) { g_spc_dbt_stats[5]++; return 0; }
  if (b->epoch != g_spc_dbt_epoch && !spc_dbt_valid(b, spc)) return 0;
  const uint32_t c0 = apu->cycles;
  const uint32_t edge0 = (32u - (c0 & 31u)) & 31u;
  s_target = (void *)b->fn;
  s_flushed = false;
#ifdef SPC_DBT_SELFCHECK
  extern uint32_t g_spc_dbt_edges;
  const uint32_t e0 = g_spc_dbt_edges;
#endif
  const uint32_t used = ((EnterFn)(void *)&s_code[s_enter])(spc, c0, edge0, limit);
#ifdef SPC_DBT_SELFCHECK
  if (used > limit) {
    extern void n64_platform_log(const char *fmt, ...);
    static int over;
    if (over++ < 10) n64_platform_log("[dbtcheck] over: pc %04x -> %04x limit %lu used %lu\n", pc, spc->pc,
                                      (unsigned long)limit, (unsigned long)used);
  }
  { /* Self-check: the edges run are those at cycles in [c0, c0 + used). */
    const uint32_t want = used ? ((c0 + used - 1) >> 5) - ((c0 + 31) >> 5) + 1 : 0;
    const uint32_t got = g_spc_dbt_edges - e0;
    static int bad;
    if (got != (used && ((c0 + 31) >> 5) <= ((c0 + used - 1) >> 5) ? want : 0) && bad++ < 30) {
      extern void n64_platform_log(const char *fmt, ...);
      n64_platform_log("[dbtcheck] pc %04x -> %04x c0 %lu limit %lu used %lu edges %lu want %lu\n", pc, spc->pc,
                       (unsigned long)c0, (unsigned long)limit, (unsigned long)used, (unsigned long)got,
                       (unsigned long)want);
      if (bad < 4) {
        n64_platform_log("[dbtcheck]  bytes");
        for (uint16_t a = pc; a != (uint16_t)(spc->pc + 4); a++) n64_platform_log(" %02x", apu->ram[a]);
        n64_platform_log("\n");
      }
    }
  }
#endif
#ifdef SPC_DBT_DUMP
  { extern void n64_platform_log(const char *fmt, ...);
    static int runs;
    if (runs++ < 0)
      n64_platform_log("DBTRUN pc %04x -> %04x limit %lu max %u used %lu c0 %lu edge0 %lu a%02x x%02x y%02x sp%02x\n", pc, spc->pc,
                       (unsigned long)limit, b->max_cycles, (unsigned long)used, (unsigned long)c0,
                       (unsigned long)edge0, spc->a, spc->x, spc->y, spc->sp); }
#endif
  g_spc_dbt_stats[2]++;
  g_spc_dbt_stats[3] += used;
  return used;
}

#endif
