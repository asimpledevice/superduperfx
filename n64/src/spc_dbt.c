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

enum { kCodeWords = 32768, kBlocks = 1536, kMaxInsns = 24, kMapPages = 48 };
static uint32_t s_code[kCodeWords] __attribute__((aligned(32)));
static uint32_t s_ncode, s_epilogue, s_exit, s_edge, s_fe, s_prologue, s_chain;
/* Bumped whenever a block's entry conditions may have changed (page marks,
 * the direct page, translated code): a block validated in the current
 * epoch may be entered without checking again, so blocks chain. */
uint32_t g_spc_dbt_epoch = 1;
/* Block ends: 0 an exit that does not chain (leaving before an instruction),
 * 1 no map page, 2 no block (or untranslatable), 3 stale epoch, 4 no time
 * left, 5 chained. 6: edges that ended a block (batch change). */
uint32_t g_spc_dbt_chainend[8];
static bool s_have_epilogue;
typedef uint32_t (*BlockFn)(Spc *spc, uint32_t c0, uint32_t edge0, uint32_t limit);
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

void spc_dbt_flush(void) {
  memset(g_spc_dbt_map, 0, sizeof g_spc_dbt_map);
  memset(g_spc_dbt_code, 0, sizeof g_spc_dbt_code);
  s_ncode = s_nblocks = s_npages = 0;
  s_have_epilogue = false;
  g_spc_dbt_epoch++;
  g_spc_dbt_stats[1]++;
}

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
       REDGE = T9 };
/* The frame: 32 bytes of argument home, saved registers, then locals. */
enum { FRAME = 160, F_C0 = 112, F_EDGE = 116, F_FE = 120, F_TMP = 124, F_RET = 128,
       F_SAVE0 = 132, F_SAVE3 = 136, F_LIMIT = 148, F_USED = 152, F_CHAIN = 156 };   /* F_TMP + 20: edge flag */

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

/* A branch to a fixed word of the buffer (the shared routines). */
static void br_to(int op, int rs, int rt, uint32_t to) {
  i_(op, rs, rt, (int)(to - (s_ncode + 1)));
}
static void jal_code(uint32_t word_index) {
  put(3u << 26 | (((uint32_t)(uintptr_t)&s_code[word_index] >> 2) & 0x03ffffff));
}

/* The routines every block shares, at the start of the buffer:
 *   epilogue  stores the SPC registers, returns v0 (pc already stored)
 *   exit      t0 = pc << 16 | cycles: stores pc, handles a sample edge
 *             before that cycle, then the epilogue
 *   edge      (jal) t0 = next pc << 16 | its cycle: handles the sample edge
 *             at t9; returns, or ends the block at that instruction when
 *             the edge started or synced an S-DSP batch
 *   fe        (jal) catches the S-DSP front end up before an ARAM store
 *   prologue  (jal, after the block saved ra) saves s0-s8 and loads the
 *             SPC state: a0 spc, a1 the start cycle, a2 the first edge */
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

  /* exit: every sample edge before the exit cycle, then the epilogue. */
  /* exit and chain share the edge loop; exit then returns the cycles of
   * the whole chain. */
  s_exit = s_ncode;
  SW(ZERO, F_CHAIN, SP);           /* no chaining */
  const uint32_t exit_common = s_ncode;
  SRL(T2, T0, 16);
  SH(T2, offsetof(Spc, pc), RSPC);
  ANDI(V0, T0, 0xffff);
  SW(V0, F_TMP, SP);
  const uint32_t exit_loop = s_ncode;
  LW(V0, F_TMP, SP);
  SLT(T1, REDGE, V0);
  const uint32_t exit_test = s_ncode;
  i_(4, T1, ZERO, 0);                /* beq t1, zero, edges done (patched) */
  NOP();
  LW(A1, F_C0, SP);
  ADDU(A1, A1, REDGE);
  ADDIU(REDGE, REDGE, 32);
  SW(REDGE, F_EDGE, SP);
  LW(A0, offsetof(Spc, apu), RSPC);
  jal((void *)spc_dbt_edge);
  NOP();
  LW(REDGE, F_EDGE, SP);
  br_to(4, ZERO, ZERO, exit_loop);
  NOP();
  s_code[exit_test] |= (uint32_t)(s_ncode - (exit_test + 1)) & 0xffff;
  /* Edges done: v0 = this block's cycles. Chain to the next block if it is
   * translated, valid in this epoch and fits in the time left. */
  LW(T1, F_USED, SP);
  ADDU(T1, T1, V0);                  /* the chain's cycles */
  LW(T2, F_CHAIN, SP);
  const uint32_t no_chain0 = s_ncode;
  i_(4, T2, ZERO, 0);                /* beq: exits do not chain */
  NOP();
  LHU_(T2, offsetof(Spc, pc), RSPC);
  SRL(T4, T2, 8);
  SLL(T4, T4, 2);
  li(T5, (uint32_t)(uintptr_t)g_spc_dbt_map);
  ADDU(T5, T5, T4);
  LW(T5, 0, T5);                     /* the map page */
  const uint32_t no_chain1 = s_ncode;
  i_(4, T5, ZERO, 0);
  NOP();
  ANDI(T4, T2, 0xff);
  SLL(T4, T4, 2);
  ADDU(T5, T5, T4);
  LW(T5, 0, T5);                     /* the block, or NULL / 1 */
  SLTIU(T6, T5, 2);
  const uint32_t no_chain2 = s_ncode;
  i_(5, T6, ZERO, 0);
  NOP();
  LW(T6, offsetof(Block, epoch), T5);
  li(T7, (uint32_t)(uintptr_t)&g_spc_dbt_epoch);
  LW(T7, 0, T7);
  const uint32_t no_chain3 = s_ncode;
  i_(5, T6, T7, 0);
  NOP();
  const uint32_t fits_check = s_ncode;
  LHU_(T6, offsetof(Block, max_cycles), T5);
  LW(T7, F_LIMIT, SP);
  SUBU(T7, T7, T1);                  /* time left */
  SLTU(T7, T7, T6);
  const uint32_t no_chain4 = s_ncode;
  i_(5, T7, ZERO, 0);
  NOP();
  SW(T1, F_USED, SP);
  LW(T7, F_C0, SP);
  ADDU(T7, T7, V0);
  SW(T7, F_C0, SP);
  SUBU(REDGE, REDGE, V0);
  SW(REDGE, F_EDGE, SP);
  li(T7, (uint32_t)(uintptr_t)&g_spc_dbt_chainend[5]);
  LW(T6, 0, T7);
  ADDIU(T6, T6, 1);
  SW(T6, 0, T7);
  LW(T5, offsetof(Block, fn), T5);
  ADDIU(T5, T5, 16);                 /* past its prologue call */
  JR(T5);
  NOP();
  /* Why chains end (statistics): one counter per branch above. */
  const uint32_t fix[] = { no_chain0, no_chain1, no_chain2, no_chain3, no_chain4 };
  uint32_t reason_at[5];
  for (int i = 0; i < 5; i++) {
    reason_at[i] = s_ncode;
    li(T7, (uint32_t)(uintptr_t)&g_spc_dbt_chainend[i]);
    LW(T6, 0, T7);
    ADDIU(T6, T6, 1);
    SW(T6, 0, T7);
    MOVE(V0, T1);
    j_(s_epilogue);
    NOP();
  }
  /* A stale block: validate it now (spc_dbt_valid) instead of leaving
   * translated code; chain on if it may run. */
  const uint32_t stale = s_ncode;
  SW(T1, F_SAVE0, SP);
  SW(T5, F_SAVE3, SP);
  MOVE(A0, T5);
  MOVE(A1, RSPC);
  jal((void *)spc_dbt_valid);
  NOP();
  MOVE(T6, V0);
  LW(T1, F_SAVE0, SP);
  LW(T5, F_SAVE3, SP);
  LW(V0, F_TMP, SP);
  LW(REDGE, F_EDGE, SP);
  br_to(4, T6, ZERO, reason_at[3]);
  NOP();
  br_to(4, ZERO, ZERO, fits_check);
  NOP();
  for (int i = 0; i < 5; i++)
    s_code[fix[i]] |= (uint32_t)((i == 3 ? stale : reason_at[i]) - (fix[i] + 1)) & 0xffff;
  s_chain = s_ncode;
  ORI(T1, ZERO, 1);
  SW(T1, F_CHAIN, SP);
  br_to(4, ZERO, ZERO, exit_common);
  NOP();

  /* edge (jal): every sample edge before t0's cycle; if one started or
   * synced an S-DSP batch, the block ends at t0's instruction (not run). */
  s_edge = s_ncode;
  SW(RA, F_RET, SP);
  SW(T0, F_TMP, SP);
  SW(ZERO, F_TMP + 20, SP);     /* any batch change */
  const uint32_t edge_loop = s_ncode;
  LW(T0, F_TMP, SP);
  ANDI(T0, T0, 0xffff);
  SLT(T1, REDGE, T0);
  const uint32_t edge_test = s_ncode;
  i_(4, T1, ZERO, 0);           /* beq t1, zero, done (patched below) */
  NOP();
  LW(A1, F_C0, SP);
  ADDU(A1, A1, REDGE);
  ADDIU(REDGE, REDGE, 32);
  SW(REDGE, F_EDGE, SP);
  LW(A0, offsetof(Spc, apu), RSPC);
  jal((void *)spc_dbt_edge);
  NOP();
  LW(T1, F_TMP + 20, SP);
  OR(T1, T1, V0);
  SW(T1, F_TMP + 20, SP);
  LW(REDGE, F_EDGE, SP);
  ORI(T1, ZERO, 1);
  SW(T1, F_FE, SP);
  br_to(4, ZERO, ZERO, edge_loop);
  NOP();
  s_code[edge_test] |= (uint32_t)(s_ncode - (edge_test + 1)) & 0xffff;
  LW(T1, F_TMP + 20, SP);
  i_(5, T1, ZERO, 3);           /* bne t1, zero, +3: a batch changed: end here */
  LW(RA, F_RET, SP);
  JR(RA);
  NOP();
  LW(T0, F_TMP, SP);
  SRL(T2, T0, 16);
  SH(T2, offsetof(Spc, pc), RSPC);
  ANDI(V0, T0, 0xffff);
  LW(T1, F_USED, SP);
  ADDU(V0, V0, T1);
  j_(s_epilogue);
  NOP();

  s_fe = s_ncode;
  SW(RA, F_RET, SP);
  LW(A0, offsetof(Spc, apu), RSPC);
  jal((void *)spc_dbt_fe);
  NOP();
  SW(ZERO, F_FE, SP);
  LW(REDGE, F_EDGE, SP);
  LW(RA, F_RET, SP);
  JR(RA);
  NOP();

  s_prologue = s_ncode;
  for (int i = 0; i < 9; i++) SD(kSaved[i], 32 + 8 * i, SP);
  MOVE(RSPC, A0);
  SW(A1, F_C0, SP);
  MOVE(REDGE, A2);
  SW(A2, F_EDGE, SP);
  SW(A3, F_LIMIT, SP);
  SW(ZERO, F_USED, SP);
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
  JR(RA);
  ADDU(RRAM, T0, T1);
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
} Ctx;

static void add_page(Block *b, uint8_t page, uint8_t flag) {
  for (int i = 0; i < b->npages; i++)
    if (b->page[i] == page) { b->pflag[i] |= flag; return; }
  if (b->npages < 10) { b->page[b->npages] = page; b->pflag[b->npages++] = flag; }
  else s_full = true;   /* too many pages: give up on the block */
}
static void add_range(Block *b, uint16_t lo, uint16_t hi, uint8_t flag) {
  add_page(b, (uint8_t)(lo >> 8), flag);
  if ((lo >> 8) != (hi >> 8)) add_page(b, (uint8_t)(hi >> 8), flag);
  if (flag & 2) {
    if (b->nw < 6) { b->wlo[b->nw] = lo; b->whi[b->nw++] = hi; }
    else s_full = true;
  }
}

static inline bool is_io(uint16_t adr) { return (adr & 0xfff0) == 0x00f0; }
static inline bool is_rom(uint16_t adr) { return adr >= 0xffc0; }

/* Call a runtime helper with apu in a0, the instruction's start cycle in
 * a3 (io_write) or a2 (io_read). Callers have set a1/a2. */
static void call_cycle(Ctx *c, void *fn, int cyc_reg) {
  LW(cyc_reg, F_C0, SP);
  ADDIU(cyc_reg, cyc_reg, c->o);
  LW(A0, offsetof(Spc, apu), RSPC);
  jal(fn);
  NOP();
  LW(REDGE, F_EDGE, SP);   /* t9 does not survive calls */
}

typedef struct { int kind, label, resume; uint16_t pc, o, adr; uint8_t where, src; } Stub;
static Stub s_stub[160];
static int s_nstub;

/* The sample edges before this instruction, before its first store or I/O
 * access (only those see the time): hot test inline, the work out of line;
 * a batch change ends the block before the instruction. */
static void edge_sync(Ctx *c) {
  if (c->edges_synced) return;
  c->edges_synced = true;
  const int cold = new_label(), back = new_label();
  SLTI(T1, REDGE, c->o);
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
typedef struct { bool dyn; uint16_t adr; } Addr;
enum { M_DP = 1, M_ABS = 2, M_IND = 3, M_IMM = 5, M_DPX = 6, M_ABX = 7, M_ABY = 8, M_DPY = 10 };

/* Returns false if the mode is not supported. len receives the operand
 * bytes. flag: 1 read, 2 written, 3 both. */
static bool addr_of(Ctx *c, int mode, Addr *a, int *len, uint8_t flag) {
  const uint8_t *ram = c->ram;
  const uint16_t pc = c->pc, pg = (uint16_t)(c->p << 8);
  const uint8_t b1 = ram[(uint16_t)(pc + 1)], b2 = ram[(uint16_t)(pc + 2)];
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
      if (mode == M_IND) MOVE(T0, idx);
      else { ADDIU(T0, idx, b1); ANDI(T0, T0, 0xff); }
      if (!c->p) {   /* page 0: the I/O registers are at $F0-$FF */
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
    case M_ABX: case M_ABY: {
      const uint16_t base = (uint16_t)(b1 | b2 << 8);
      a->dyn = true; *len = 2;
      li(T1, base);
      ADDU(T0, mode == M_ABY ? RY : RX, T1);
      ANDI(T0, T0, 0xffff);
      const uint16_t hi = (uint16_t)(base + 255);
      const bool wraps = hi < base;
      if (wraps || (base <= 0xff && hi >= 0xf0)) {   /* can reach the I/O page */
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
static void rd(Ctx *c, const Addr *a, int dst) {
  if (a->dyn) {
    ADDU(T1, RRAM, T0);
    LBU(dst, -0x8000, T1);
  } else if (is_io(a->adr)) {
    edge_sync(c);
    li(A1, a->adr);
    call_cycle(c, (void *)spc_dbt_io_read, A2);
    MOVE(dst, V0);
    if (a->adr >= 0xf1 && a->adr <= 0xf3) c->exit_after = true;
  } else {
    LBU(dst, (int)a->adr - 0x8000, RRAM);
  }
}
/* Write src to a (src must not be a t-register a helper call clobbers when
 * a is in the I/O page: pass t3 or an s-register; it is moved to a2 first). */
static void wr(Ctx *c, const Addr *a, int src) {
  if (a->dyn) {
    st(c, 1, 0, src);
  } else if (is_io(a->adr)) {
    edge_sync(c);
    MOVE(A2, src);
    li(A1, a->adr);
    call_cycle(c, (void *)spc_dbt_io_write, A3);
    if (a->adr >= 0xf1 && a->adr <= 0xf3) c->exit_after = true;
  } else {
    st(c, 0, a->adr, src);
  }
}
static void set_zn(int r) { MOVE(RZ, r); MOVE(RN, r); }

/* Ends the block: pc (in t0, 16 bits) and the cycles used; the shared
 * exit handles a sample edge before that cycle. */
static void exit_to(Ctx *c, uint32_t used) {
  (void)c;
  SLL(T0, T0, 16);
  ORI(T0, T0, (int)used);
  j_(s_chain);
  NOP();
}
static void exit_const(Ctx *c, uint16_t pc, uint32_t used) {
  (void)c;
  li(T0, (uint32_t)pc << 16 | used);
  j_(s_chain);
  NOP();
}

/* A conditional branch at the end of the block: taken when t1 != 0
 * (want_set) or t1 == 0. */
static void add_tgt(Block *b, uint16_t pc) {
  if (b->ntgt < 2) b->tgt[b->ntgt++] = pc;
}
static void branch_end(Ctx *c, int len, int8_t rel, uint32_t base_cycles, bool want_set) {
  const uint16_t next = (uint16_t)(c->pc + len), target = (uint16_t)(next + rel);
  add_tgt(c->b, next);
  add_tgt(c->b, target);
  const int taken = new_label();
  if (want_set) BNE(T1, ZERO, taken); else BEQ(T1, ZERO, taken);
  NOP();
  exit_const(c, next, c->o + base_cycles);
  bind(taken);
  exit_const(c, target, c->o + base_cycles + 2);
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
  if (!addr_of(c, mode, &a, len, flag)) return false;
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
      wr(c, &a, r);
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

/* One instruction. Returns: 0 not translated, 1 translated (the block
 * goes on, or ends after it when c->exit_after is set), 2 translated and
 * ends the block (it emitted its exits). */
static int emit_insn(Ctx *c, uint8_t op, int *len) {
  const uint8_t b1 = c->ram[(uint16_t)(c->pc + 1)];
  const uint32_t cyc = (uint32_t)s_cyc[op];
  *len = 0;
  if (emit_generic(c, op, len)) { (*len)++; return 1; }
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
      ANDI(T8, RY, 0xf);
      SLTU(T7, T8, T7);
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
    case 0x2f:                                               /* bra */
      add_tgt(c->b, (uint16_t)(c->pc + 2 + (int8_t)b1));
      exit_const(c, (uint16_t)(c->pc + 2 + (int8_t)b1), c->o + cyc);
      return 2;
    case 0x5f:                                               /* jmp !abs */
      add_tgt(c->b, (uint16_t)(b1 | c->ram[(uint16_t)(c->pc + 2)] << 8));
      exit_const(c, (uint16_t)(b1 | c->ram[(uint16_t)(c->pc + 2)] << 8), c->o + cyc);
      return 2;
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
static Block *translate(Apu *apu, uint16_t pc, uint8_t p) {
  if (!s_cyc) spc_tables(&s_cyc, &s_mode, &s_op);
  if (pc < 0x200 || pc >= 0xff00) return NULL;
  if (s_nblocks == kBlocks || s_ncode + 1024 > kCodeWords) {
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
  }
  const uint32_t start = s_ncode;
  Block *b = &s_blocks[s_nblocks];
  memset(b, 0, sizeof *b);
  b->pc = pc;
  b->p = p;
  s_full = false;
  s_nlabels = s_nfix = 0;
  Ctx c = { b, apu->ram, p, pc, 0, 0, false, false };
  /* Prologue: the frame, ra, then the shared part. */
  ADDIU(SP, SP, -FRAME);
  SD(RA, 32 + 8 * 9, SP);
  jal_code(s_prologue);
  NOP();
  s_nstub = 0;

  int n = 0, ended = 0;
  uint32_t max = 0;
  uint16_t end_pc = pc;

  while (n < kMaxInsns) {
    const uint8_t op = c.ram[c.pc];
    if (c.pc >= 0xff00) break;
    c.bail = new_label();
    c.exit_after = false;
    c.fe_checked = false;
    c.edges_synced = false;
    c.bail_used = false;
    const uint32_t ncode0 = s_ncode;
    const int nfix0 = s_nfix;
    const uint8_t npages0 = b->npages, nw0 = b->nw;
    const int nstub0 = s_nstub;
    int len = 0;
    const int r = emit_insn(&c, op, &len);
    if (!r) {                     /* not covered: drop what it emitted */
      s_ncode = ncode0;
      s_nfix = nfix0;
      b->npages = npages0;
      b->nw = nw0;
      s_nstub = nstub0;
      break;
    }
    /* The instruction's code bytes are read from ARAM. */
    add_range(b, c.pc, (uint16_t)(c.pc + len - 1), 1);
    const uint16_t next = (uint16_t)(c.pc + len);
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
    end_pc = next;
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
  /* The longest path: a block never runs unless it fits in the time left. */
  b->max_cycles = (uint16_t)(ended ? max : c.o);
  /* The cold paths. */
  for (int i = 0; i < s_nstub; i++) {
    const Stub *st = &s_stub[i];
    bind(st->label);
    if (st->kind == 0) {          /* leave before the instruction */
      li(T0, (uint32_t)st->pc << 16 | st->o);
      j_(s_exit);
      NOP();
    } else if (st->kind == 1) {   /* the sample edges before an instruction */
      li(T0, (uint32_t)st->pc << 16 | st->o);
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
      LW(A3, F_C0, SP);
      ADDIU(A3, A3, st->o);
      LW(A0, offsetof(Spc, apu), RSPC);
      jal((void *)spc_dbt_slow_write);
      NOP();
      LW(REDGE, F_EDGE, SP);
      LW(T0, F_SAVE0, SP);
      LW(T3, F_SAVE3, SP);
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
    if (dumped++ < 0) {
      n64_platform_log("DBTBLOCK pc %04x at %08lx words %lu epi %08lx spc", pc, (unsigned long)(uintptr_t)&s_code[start],
                       (unsigned long)(s_ncode - start), (unsigned long)(uintptr_t)&s_code[s_epilogue]);
      for (uint16_t a = pc; a != end_pc; a++) n64_platform_log(" %02x", c.ram[a]);
      n64_platform_log("\n");
      for (uint32_t i = start; i < s_ncode; i++) n64_platform_log("DBTW %08lx\n", (unsigned long)s_code[i]);
    }
  }
#endif
  /* Mark its code lines, so stores to them flush. */
  for (uint16_t a = pc; a != end_pc; a++)
    g_spc_dbt_code[a >> 8] |= (uint8_t)(1u << ((a >> 5) & 7));
  s_nblocks++;
  g_spc_dbt_epoch++;   /* its code lines are new: stores elsewhere must recheck */
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
  b->epoch = g_spc_dbt_epoch;
  return 1;
}

/* Translate at pc and record the result (or that there is none) in the map. */
static Block *translate_at(Apu *apu, uint16_t pc, uint8_t p) {
  Block *b = translate(apu, pc, p);
  void **pg = map_page((uint8_t)(pc >> 8));   /* a flush may have cleared the map */
  if (!pg) return NULL;
  pg[pc & 255] = b ? (void *)b : (void *)1;
  return b;
}

uint32_t spc_dbt_run(Apu *apu, uint32_t limit) {
  Spc *const spc = apu->spc;
  const uint16_t pc = spc->pc;
  void **pg = map_page((uint8_t)(pc >> 8));
  if (!pg) return 0;
  Block *b = (Block *)pg[pc & 255];
  if (!b) {
    const uint32_t flushes = g_spc_dbt_stats[1];
    b = translate_at(apu, pc, spc->p);
    if (!b) { g_spc_dbt_stats[6]++; return 0; }
    (void)flushes;
  }
  if ((void *)b == (void *)1) return 0;
  if (b->max_cycles > limit) { g_spc_dbt_stats[5]++; return 0; }
  if (b->epoch != g_spc_dbt_epoch && !spc_dbt_valid(b, spc)) return 0;
  const uint32_t c0 = apu->cycles;
  const uint32_t edge0 = (32u - (c0 & 31u)) & 31u;
  const uint32_t used = b->fn(spc, c0, edge0, limit);
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
