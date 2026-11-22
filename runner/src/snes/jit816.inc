/* 65816 JIT, first version (SNESRECOMP_JIT816=1; included by interp_bridge.c).
 *
 * What it translates: a block is what the lean loop (bridge_lean_run) would
 * run without leaving -- instructions in ROM, forward, in one bank and one
 * 8 KB page, none of them a call, return, jump or halt. A block is entered
 * at any of its instructions, runs natively, and leaves:
 *   - when the clock reaches s_lean_event (after the instruction that did
 *     it, exactly where the lean loop would stop to settle);
 *   - at a branch out of the block (every backward branch: loop heads stay
 *     with the big loop and its wait detector);
 *   - before the first instruction it has no code for.
 * A branch back to an instruction of the block loops inside it, for a few
 * laps at a time. The lean loop interprets whatever a block leaves. A block is made the
 * second time its first instruction is reached.
 *
 * Exactness. A translated instruction does what interp816 does for it, to
 * the same registers (the reserved FPU registers, cpu_regs.h; this needs
 * SNESRECOMP_INTERP_REGS=1): same result and flags, same data accesses in the
 * same order through the same bus functions, and the same master clocks.
 * The opcode and operand bytes are not read at run time: they are ROM, so
 * their values and their bus speed are constants (the block is tied to the
 * page pointer and to g_memsel), and the two things the reads leave behind
 * -- the open-bus latch and the cartridge's bus address -- are stored before
 * anything that could observe them (a data access, leaving the block).
 * Clocks: an instruction costs its accesses' speeds plus 6 per internal
 * cycle, as _interp_run_core computes it.
 *
 * Not kept: the interpreter's tallies and the per-PC reports (diagnostics).
 *
 * Registers in a block: s0 = the Interp816 (for pc), s1 = instructions
 * finished, s2/s3/s6 = values that live across a bus call, s4 = this
 * instruction's master clocks so far, s5 = laps left in a loop. */

void data_cache_hit_writeback(volatile const void *, unsigned long);
void inst_cache_hit_invalidate(volatile void *, unsigned long);
extern const uint8_t cyclesPerOpcode[256];   /* interp816.c */

#ifndef SNESRECOMP_JIT_XFER
#define SNESRECOMP_JIT_XFER 1   /* JMP, JML, JSR, JSL, RTS, RTL in blocks */
#endif
enum { kJitCodeWords = 98304, kJitEntries = 8192, kJitMaxInsns = 32, kJitSeen = 2048,
       kJitLaps = 8,       /* laps in a row without progress before a loop inside a block leaves for the big loop */
       kJitRunMax = 4096   /* instructions after which a loop leaves in any case */ };
static uint32_t s_jit_code[kJitCodeWords] __attribute__((aligned(32)));
static uint32_t s_jit_n;           /* words used */
static uint32_t s_jit_epilogue;    /* word index */
static uint32_t s_jit_rd8, s_jit_rd16;   /* word indexes of the shared read routines */
/* The clock in a block: JR = s_lean_event - g_master_cycles as the block
 * sees it (the time left before the event; <= 0: leave after this
 * instruction). The clock register, the coprocessors' and the SPC's
 * pending time are brought up to date from it (s_jit_syncout) before
 * anything outside the block can read them -- a C bus function, the way
 * out -- and JR is taken again from them after a C function (which may
 * move the clock, as DMA does, or the event). */
#define JR JS8
static uint32_t s_jit_syncout;           /* word index: the subroutine (clobbers t0-t3) */
enum { JC_WR8, JC_WR16, JC_WR16R, JC_RD8, JC_RD16, JC_PTR24, JC_N };
static uint32_t s_jit_c[JC_N];           /* word indexes: the C bus functions, with the clock handed over and back */
/* Stores: a byte to plain WRAM is made here, as bridge_bus_write and
 * cpu_write8 make it when nothing watches stores (s_jit_store_ok, from
 * cpu_wram_store_plain at each run): the byte, its speed onto
 * s_interp_bus_master, the write epoch, the open-bus latch and the
 * cartridge's bus address. Anything else goes to the C function. In: a0
 * the address, a1 the byte (st16: a0/a1 the low and high addresses, a2 the
 * word, stored low byte first, or by the C function when the low address
 * is not WRAM: a hardware register pair is claimed whole). */
#ifndef SNESRECOMP_JIT_FAST_STORE
#define SNESRECOMP_JIT_FAST_STORE 1
#endif
static uint32_t s_jit_st8, s_jit_st16;
/* The way out of a block for an instruction that has a table entry: t0 =
 * its index, t1 = the block's table, t2 = the bank << 16, t5 = where to go
 * (-1 the next instruction: the event; -2 the same, at the block's end; else
 * the pc: a branch out). An entry is the instruction's pc | its last byte
 * << 16 | (its length - 1) << 24 | 1 << 26 when it made no bus access (the
 * bus latch is then its last byte's). A block ends with a thunk that loads
 * t1 and t2 and jumps here, and with its table. */
static uint32_t s_jit_xcommon;
static uint8_t s_jit_store_ok;
static uint8_t s_jit_speed[kCpuFastMapPages];   /* master clocks per access, by 8 KB page (RAM and ROM pages) */
typedef struct { uint32_t key; uint32_t code; const uint8_t *pg; uint32_t n; } JitEntry;   /* n: bit 31 = holds ADC/SBC (binary mode only) */
static JitEntry s_jit_ent[kJitEntries];   /* key: 0x80000000 | mx << 24 | pc24 */
static uint32_t s_jit_nent;
static uint32_t s_jit_seen[kJitSeen];     /* block starts reached once */
/* (An entry of s_jit_seen: the key = reached once; key ^ 0x40000000 = nothing
 * translatable starts there; key ^ 0x20000000 = translated at some time.
 * The last kind survives a flush, and such a block is made again the first
 * time it is reached: it earned its place already. No table of its own:
 * another 8 KB to look things up in cost Joe & Mac 2 3 ms a frame.) */
static uint8_t s_jit_memsel;
static bool s_jit_full;
static uint32_t s_jit_fp;                 /* a looping block's A, X, Y and write count at its last lap */
uint32_t g_jit816_prev;                   /* pc24 of the last instruction a block ran */
uint32_t g_jit816_ended[256];             /* by opcode: translations that stopped at it (no code for it yet) */
uint32_t g_jit816_stats[6];               /* blocks, instructions translated, runs, instructions run, flushes, refused */
/* Words emitted, by what they are for: 0 the instruction's own work, 1 its
 * step end (clock, SPC time, count, event test), 2 its exit stub (latch and
 * exit), 3 branches whole, 4 transfers whole, 5 a block's tail exit. */
uint32_t g_jit816_words[6];
static CpuState *s_jit_cpu;
static uint32_t s_jit_ret_limit;          /* a return that leaves S above this ends the run: the big loop's (0xFFFFFFFF: none) */
static BridgeTailFrame *s_jit_tail;       /* the run's call records (tail_frames in _interp_run_core) */
static int *s_jit_tail_n;
/* What the big loop does to the records for a call it interprets and for a
 * return (its tail_frames code, same order and tests). */
static void jit_tail_call(uint32_t s_after, uint32_t size, uint32_t ret_pc24) {
    BridgeTailFrame *const tf = s_jit_tail;
    int n = *s_jit_tail_n;
#if SNESRECOMP_TAIL_FRAMES_KEEP
    while (n > 0 && tf[n - 1].s_after <= (uint16_t)s_after) n--;
#endif
    if (n == kBridgeTailFrames) {
        memmove(&tf[0], &tf[1], sizeof(BridgeTailFrame) * (kBridgeTailFrames - 1));
        n--;
    }
    tf[n].s_after = (uint16_t)s_after;
    tf[n].size = (uint8_t)size;
    tf[n].ret_pc24 = ret_pc24;
    *s_jit_tail_n = n + 1;
}
static void jit_tail_ret(uint32_t sp_before, uint32_t size) {
    const int n = *s_jit_tail_n;
    if (n > 0 && s_jit_tail[n - 1].s_after == (uint16_t)sp_before && s_jit_tail[n - 1].size == (uint8_t)size)
        *s_jit_tail_n = n - 1;
    else
        *s_jit_tail_n = 0;
}
static Interp816 *s_jit_in;
typedef uint32_t (*JitEnterFn)(void *code, Interp816 *in);

enum { JZERO = 0, JAT = 1, JV0 = 2, JV1 = 3, JA0 = 4, JA1 = 5, JA2 = 6, JA3 = 7, JT0 = 8, JT1 = 9, JT2 = 10,
       JT3 = 11, JT4 = 12, JT5 = 13, JT6 = 14, JT7 = 15, JT8 = 24, JT9 = 25, JS0 = 16, JS1 = 17, JS2 = 18, JS3 = 19, JS4 = 20, JS5 = 21, JS6 = 22, JS7 = 23, JS8 = 30, JSP = 29, JRA = 31 };
enum { FA = 17, FX = 18, FY = 19, FS = 20, FD = 21, FDB = 22, FPB = 23, FPMX = 24, FMC = 26, FCMC = 27,
       FNZ = 28, FC = 29, FV = 30 };   /* fpu_slots.h */

static void jput(uint32_t w) {
    if (s_jit_n < kJitCodeWords) s_jit_code[s_jit_n++] = w; else s_jit_full = true;
}
static void jI(int op, int rs, int rt, int imm) { jput((uint32_t)op << 26 | (uint32_t)rs << 21 | (uint32_t)rt << 16 | ((uint32_t)imm & 0xffffu)); }
static void jR(int rs, int rt, int rd, int sa, int fn) { jput((uint32_t)rs << 21 | (uint32_t)rt << 16 | (uint32_t)rd << 11 | (uint32_t)sa << 6 | (uint32_t)fn); }
#define J_ADDIU(rt, rs, imm) jI(9, rs, rt, imm)
#define J_ANDI(rt, rs, imm) jI(12, rs, rt, imm)
#define J_ORI(rt, rs, imm) jI(13, rs, rt, imm)
#define J_XORI(rt, rs, imm) jI(14, rs, rt, imm)
#define J_LUI(rt, imm) jI(15, 0, rt, imm)
#define J_SLTIU(rt, rs, imm) jI(11, rs, rt, imm)
#define J_LW(rt, off, rs) jI(35, rs, rt, off)
#define J_SW(rt, off, rs) jI(43, rs, rt, off)
#define J_LBU(rt, off, rs) jI(36, rs, rt, off)
#define J_SB(rt, off, rs) jI(40, rs, rt, off)
#define J_SH(rt, off, rs) jI(41, rs, rt, off)
#define J_LD(rt, off, rs) jI(55, rs, rt, off)
#define J_SD(rt, off, rs) jI(63, rs, rt, off)
#define J_ADDU(rd, rs, rt) jR(rs, rt, rd, 0, 0x21)
#define J_SUBU(rd, rs, rt) jR(rs, rt, rd, 0, 0x23)
#define J_DADDU(rd, rs, rt) jR(rs, rt, rd, 0, 0x2d)
#define J_DSUBU(rd, rs, rt) jR(rs, rt, rd, 0, 0x2f)
#define J_SLT(rd, rs, rt) jR(rs, rt, rd, 0, 0x2a)
#define J_AND(rd, rs, rt) jR(rs, rt, rd, 0, 0x24)
#define J_OR(rd, rs, rt) jR(rs, rt, rd, 0, 0x25)
#define J_XOR(rd, rs, rt) jR(rs, rt, rd, 0, 0x26)
#define J_SLTU(rd, rs, rt) jR(rs, rt, rd, 0, 0x2b)
#define J_SLL(rd, rt, sa) jR(0, rt, rd, sa, 0)
#define J_SRL(rd, rt, sa) jR(0, rt, rd, sa, 2)
#define J_MOVE(rd, rs) J_OR(rd, rs, JZERO)
#define J_NOP() jput(0)
#define J_JR(rs) jR(rs, 0, 0, 0, 8)
#define J_MFC1_RAW(rt, fs) jput(0x44000000u | (uint32_t)(rt) << 16 | (uint32_t)(fs) << 11)
#define J_MTC1_RAW(rt, fs) jput(0x44800000u | (uint32_t)(rt) << 16 | (uint32_t)(fs) << 11)
/* In a block A, X and Y are in callee-saved registers (JCA, JCX, JCY): taken
 * from their FPU registers on the way in, written back on every way out and
 * before a C function (jit_spill_axy). While s_jit_cache is set, a move from
 * or to one of the three FPU registers is a move from or to its copy. */
#ifndef SNESRECOMP_JIT_AXY_REGS
#define SNESRECOMP_JIT_AXY_REGS 1
#endif
#define JCA 16   /* s0 */
#define JCX 21   /* s5 */
#define JCY 22   /* s6 */
static bool s_jit_cache;
static int jit_creg(int fs) { return fs == 17 ? JCA : fs == 18 ? JCX : fs == 19 ? JCY : -1; }
static void jput(uint32_t w);
static void J_MFC1(int rt, int fs) {
    const int c = s_jit_cache ? jit_creg(fs) : -1;
    if (c >= 0) jput((uint32_t)c << 21 | (uint32_t)rt << 11 | 0x25u);   /* or rt, c, zero */
    else J_MFC1_RAW(rt, fs);
}
static void J_MTC1(int rt, int fs) {
    const int c = s_jit_cache ? jit_creg(fs) : -1;
    if (c >= 0) jput((uint32_t)rt << 21 | (uint32_t)c << 11 | 0x25u);   /* or c, rt, zero */
    else J_MTC1_RAW(rt, fs);
}
#define J_DMFC1(rt, fs) jput(0x44200000u | (uint32_t)(rt) << 16 | (uint32_t)(fs) << 11)
#define J_DMTC1(rt, fs) jput(0x44a00000u | (uint32_t)(rt) << 16 | (uint32_t)(fs) << 11)
static void jli(int t, uint32_t v) {
    if (v < 0x10000u) { J_ORI(t, JZERO, (int)v); return; }
    J_LUI(t, (int)(v >> 16));
    if (v & 0xffffu) J_ORI(t, t, (int)(v & 0xffffu));
}
static void jjal(void *fn) { jput(3u << 26 | (((uint32_t)(uintptr_t)fn >> 2) & 0x03ffffffu)); }
static void jj(uint32_t word) { jput(2u << 26 | ((((uint32_t)(uintptr_t)&s_jit_code[word]) >> 2) & 0x03ffffffu)); }
/* The address of a global, as base register JAT and a 16-bit offset. */
#ifndef SNESRECOMP_JIT_BASE_REG
#define SNESRECOMP_JIT_BASE_REG 1
#endif
static int s_jb = 1;          /* the base register the last jaddr gave (JAT or JS7) */
#define JB s_jb
static uint32_t s_jit_base;   /* what s7 holds in a block: the address of a global among those blocks use */
static int jaddr_at(const void *p) {
    const uint32_t a = (uint32_t)(uintptr_t)p;
    const int lo = (int16_t)(a & 0xffffu);
    J_LUI(JAT, (int)(((a - (uint32_t)lo) >> 16) & 0xffffu));
    s_jb = JAT;
    return lo;
}
/* The same, by the block's base register (s7 = s_jit_base, set on the way
 * in) when the global is within reach of it: no instruction at all. The
 * base to use is JB, until the next jaddr. */
static int jaddr(const void *p) {
    const int32_t d = (int32_t)((uint32_t)(uintptr_t)p - s_jit_base);
    if (SNESRECOMP_JIT_BASE_REG && s_jit_base && d >= -32768 && d <= 32767) { s_jb = JS7; return (int)d; }
    return jaddr_at(p);
}

static void jjal_code(uint32_t word) { jput(3u << 26 | ((((uint32_t)(uintptr_t)&s_jit_code[word]) >> 2) & 0x03ffffffu)); }
/* A read through the shared routines: v1 = the bus latch for a slow access
 * (the instruction's last fetched byte, its address above it), a0/a1 from
 * rlo/rhi. */
static void jit_read(bool byte, int rlo, int rhi, uint32_t last_adr, uint8_t last_byte) {
    jli(JV1, (last_adr & 0xFFFFFFu) << 8 | last_byte);
    if (!byte) J_MOVE(JA1, rhi);
    jjal_code(byte ? s_jit_rd8 : s_jit_rd16);
    J_MOVE(JA0, rlo);
}

/* Branches inside a block, to labels bound later. */
enum { kJitLabels = 160, kJitFix = 200 };
static int32_t s_jit_label[kJitLabels];
static int s_jit_nlabels;
static struct { uint32_t at; int label; } s_jit_fix[kJitFix];
static int s_jit_nfix;
static int jlabel(void) {
    if (s_jit_nlabels >= kJitLabels) { s_jit_full = true; return 0; }
    s_jit_label[s_jit_nlabels] = -1;
    return s_jit_nlabels++;
}
static void jbind(int l) { s_jit_label[l] = (int32_t)s_jit_n; }
static void jbr(int op, int rs, int rt, int l) {   /* BEQ 4, BNE 5; the caller emits the delay slot */
    if (s_jit_nfix >= kJitFix) { s_jit_full = true; return; }
    s_jit_fix[s_jit_nfix].at = s_jit_n; s_jit_fix[s_jit_nfix].label = l; s_jit_nfix++;
    jI(op, rs, rt, 0);
}
static void jfix_all(void) {
    for (int i = 0; i < s_jit_nfix; i++) {
        const int32_t to = s_jit_label[s_jit_fix[i].label];
        if (to < 0) { s_jit_full = true; continue; }
        s_jit_code[s_jit_fix[i].at] |= (uint32_t)(to - (int32_t)s_jit_fix[i].at - 1) & 0xffffu;
    }
}

/* Bus calls from a block: the functions interp816 reaches the bus through. */
static uint32_t jit_rd8(uint32_t adr) { return bridge_bus_read(s_jit_cpu, adr); }
static uint32_t jit_rd16(uint32_t lo, uint32_t hi) {
    uint16_t v;
    if (bridge_bus_read_word(s_jit_cpu, lo, hi, &v)) return v;
    const uint32_t b = bridge_bus_read(s_jit_cpu, lo);
    return b | (uint32_t)bridge_bus_read(s_jit_cpu, hi) << 8;
}
static void jit_wr8(uint32_t adr, uint32_t v) { bridge_bus_write(s_jit_cpu, adr, (uint8_t)v); }
static void jit_wr16(uint32_t lo, uint32_t hi, uint32_t v) {
    if (bridge_bus_write_word(s_jit_cpu, lo, hi, (uint16_t)v, false)) return;
    bridge_bus_write(s_jit_cpu, lo, (uint8_t)v);
    bridge_bus_write(s_jit_cpu, hi, (uint8_t)(v >> 8));
}

static void jit_wr16r(uint32_t lo, uint32_t hi, uint32_t v) {   /* a read-modify-write's write-back: high byte first */
    bridge_bus_write(s_jit_cpu, hi, (uint8_t)(v >> 8));
    bridge_bus_write(s_jit_cpu, lo, (uint8_t)v);
}
static uint32_t jit_ptr24(uint32_t a0, uint32_t a1, uint32_t a2) {   /* [dp]: the 24-bit pointer */
    return jit_rd16(a0, a1) | jit_rd8(a2) << 16;
}

/* A block's stack frame (set up on the way in). */
enum { FRAME = 192, F_RA2 = 40, F_RA3 = 144, F_T0 = 128, F_T1 = 136, F_RA4 = 152, F_BM = 160, F_LAPS = 168, F_RMW = 176 };   /* 0-39: the called C functions' argument area; 40: a routine's return address */
static void jit_syncout_body(void) {   /* (t0-t3) */
    { const int lo = jaddr(&s_lean_event); J_LD(JT0, lo, JB); }
    J_DSUBU(JT0, JT0, JR);                 /* the clock now */
    J_DMFC1(JT1, FMC);
    J_DSUBU(JT2, JT0, JT1);                /* since it was last written */
    J_DMTC1(JT0, FMC);
    J_DMTC1(JT0, FCMC);
    { const int lo = jaddr(&s_apu_pending_master); J_LD(JT3, lo, JB); J_DADDU(JT3, JT3, JT2); J_SD(JT3, lo, JB); }
}
static void jit_spill_axy(void) {      /* the FPU registers from their copies */
    if (!SNESRECOMP_JIT_AXY_REGS) return;
    J_MTC1_RAW(JCA, FA); J_MTC1_RAW(JCX, FX); J_MTC1_RAW(JCY, FY);
}
static void jit_syncin(void) {         /* (t0, t1) */
    { const int lo = jaddr(&s_lean_event); J_LD(JT0, lo, JB); }
    J_DMFC1(JT1, FMC);
    J_DSUBU(JR, JT0, JT1);
}
static void jit816_flush(void) {
    memset(s_jit_ent, 0, sizeof s_jit_ent);
    for (unsigned i = 0; i < kJitSeen; i++)   /* (blocks translated before stay known) */
        if ((s_jit_seen[i] & 0x60000000u) != 0x20000000u) s_jit_seen[i] = 0;
    s_jit_nent = 0;
    s_jit_n = 0;
    s_jit_nlabels = 0; s_jit_nfix = 0;   /* (the shared routines' labels: fixed at the end) */
    s_jit_full = false;
    s_jit_memsel = g_memsel;
    g_jit816_stats[4]++;
    /* The way in and out, shared by every block. */
    s_jit_base = 0;
    J_ADDIU(JSP, JSP, -FRAME);
    J_SD(JRA, 120, JSP); J_SD(JS0, 112, JSP); J_SD(JS1, 104, JSP); J_SD(JS2, 96, JSP);
    J_SD(JS3, 88, JSP); J_SD(JS4, 80, JSP); J_SD(JS5, 72, JSP); J_SD(JS6, 64, JSP); J_SD(JS7, 56, JSP);
    J_SD(JS8, 48, JSP);
    J_LUI(JS7, (int)(((uint32_t)(uintptr_t)&s_interp_bus_master >> 16) & 0xffffu));
    J_ORI(JS7, JS7, (int)((uint32_t)(uintptr_t)&s_interp_bus_master & 0xffffu));
    s_jit_base = (uint32_t)(uintptr_t)&s_interp_bus_master;   /* (from here on) */
    jit_syncin();
    J_ORI(JT0, JZERO, kJitLaps);
    J_SD(JT0, F_LAPS, JSP);
    if (SNESRECOMP_JIT_AXY_REGS) { J_MFC1_RAW(JCA, FA); J_MFC1_RAW(JCX, FX); J_MFC1_RAW(JCY, FY); }
    J_JR(JA0);
    J_MOVE(JS1, JZERO);
    s_jit_epilogue = s_jit_n;
    J_MOVE(JV0, JS1);
    J_LD(JRA, 120, JSP); J_LD(JS0, 112, JSP); J_LD(JS1, 104, JSP); J_LD(JS2, 96, JSP);
    J_LD(JS3, 88, JSP); J_LD(JS4, 80, JSP); J_LD(JS5, 72, JSP); J_LD(JS6, 64, JSP); J_LD(JS7, 56, JSP);
    J_LD(JS8, 48, JSP);
    J_JR(JRA);
    J_ADDIU(JSP, JSP, FRAME);
    /* The clock out (see JR): g_master_cycles = event - JR, the
     * coprocessors' clock with it, the SPC's pending time by the difference. */
    s_jit_syncout = s_jit_n;
    jit_syncout_body();
    J_JR(JRA);
    J_NOP();
    /* The C bus functions: the clock out before, JR again after. */
    { void *const fns[JC_N] = { (void *)jit_wr8, (void *)jit_wr16, (void *)jit_wr16r, (void *)jit_rd8, (void *)jit_rd16, (void *)jit_ptr24 };
      for (int k = 0; k < JC_N; k++) {
        s_jit_c[k] = s_jit_n;
        J_SD(JRA, F_RA2, JSP);
        jit_syncout_body();
        jit_spill_axy();
        jjal(fns[k]);
        J_NOP();
        jit_syncin();
        J_LD(JRA, F_RA2, JSP);
        J_JR(JRA);
        J_NOP();
      } }
    /* The stores (see s_jit_st8). They use t6-t9 and at. v1 = the bus
     * latch a slow store must find (the instruction's last fetched byte, its
     * address above it; 0: leave the latch), t5 = the instruction's pc for
     * the write log's two globals (0: leave them). A fast store adds its
     * speed to s4; a slow one adds what the C function put on
     * s_interp_bus_master and leaves that as it found it. */
    for (int word = 0; word < 2; word++) {
        if (word) s_jit_st16 = s_jit_n; else s_jit_st8 = s_jit_n;
        const int lslow = jlabel(), l1 = jlabel(), l2 = jlabel();
        J_SRL(JT8, JA0, kCpuFastMapPageShift);
        { const int lo = jaddr_at(g_cpu_fast_page_kind); J_ADDU(JAT, JAT, JT8); J_LBU(JT9, lo, JAT); }
        J_ADDIU(JT9, JT9, -(int)kCpuFastMapWram);
        J_SLTIU(JT9, JT9, 2);                              /* WRAM or its low mirror */
        { const int lo = jaddr(&s_jit_store_ok); J_LBU(JT6, lo, JB); }
        J_AND(JT9, JT9, JT6);
        if (!SNESRECOMP_JIT_FAST_STORE) J_MOVE(JT9, JZERO);
        jbr(4, JT9, JZERO, lslow);
        J_NOP();
        if (!word) {
            J_SLL(JT9, JT8, 2);
            { const int lo = jaddr_at(g_cpu_fast_read_page); J_ADDU(JAT, JAT, JT9); J_LW(JT9, lo, JAT); }
            J_ANDI(JT6, JA0, 0x1fff);
            J_ADDU(JT9, JT9, JT6);
            J_SB(JA1, 0, JT9);
            { const int lo = jaddr_at(s_jit_speed); J_ADDU(JAT, JAT, JT8); J_LBU(JT8, lo, JAT); }
            J_ADDU(JS4, JS4, JT8);
            { const int lo = jaddr(&g_open_bus); J_SB(JA1, lo, JB); }
            { const int lo = jaddr(&g_interp_bridge_write_epoch); J_LD(JT6, lo, JB); jI(25, JT6, JT6, 1); J_SD(JT6, lo, JB); }
            { const int lo = jaddr(&g_snes); J_LW(JT9, lo, JB); }
            J_LW(JT9, (int)offsetof(Snes, cart), JT9);
            J_JR(JRA);
            J_SW(JA0, (int)offsetof(Cart, cpuBusAddress), JT9);
        } else {
            const int l3 = jlabel();
            J_SD(JRA, F_RA3, JSP);
            J_SD(JA1, F_T0, JSP);
            J_SD(JA2, F_T1, JSP);
            jbr(4, JT5, JZERO, l3);
            J_NOP();
            { const int lo = jaddr(&g_interp_wlog_pc24); J_SW(JT5, lo, JB); }
            { const int lo = jaddr(&g_interp816_cur_pc); J_SW(JT5, lo, JB); }
            jbind(l3);
            J_MOVE(JT5, JZERO);
            jjal_code(s_jit_st8);
            J_MOVE(JA1, JA2);                              /* the low byte (to the low address in a0) */
            J_LD(JA0, F_T0, JSP);
            J_LD(JA1, F_T1, JSP);
            J_MOVE(JV1, JZERO);
            J_MOVE(JT5, JZERO);
            jjal_code(s_jit_st8);
            J_SRL(JA1, JA1, 8);
            J_LD(JRA, F_RA3, JSP);
            J_JR(JRA);
            J_NOP();
        }
        jbind(lslow);
        J_SD(JRA, F_RA4, JSP);
        jbr(4, JV1, JZERO, l1);
        J_NOP();
        { const int lo = jaddr(&g_open_bus); J_SB(JV1, lo, JB); }
        J_SRL(JT6, JV1, 8);
        { const int lo = jaddr(&g_snes); J_LW(JT9, lo, JB); }
        J_LW(JT9, (int)offsetof(Snes, cart), JT9);
        J_SW(JT6, (int)offsetof(Cart, cpuBusAddress), JT9);
        jbind(l1);
        jbr(4, JT5, JZERO, l2);
        J_NOP();
        { const int lo = jaddr(&g_interp_wlog_pc24); J_SW(JT5, lo, JB); }
        { const int lo = jaddr(&g_interp816_cur_pc); J_SW(JT5, lo, JB); }
        jbind(l2);
        { const int lo = jaddr(&s_interp_bus_master); J_LD(JT6, lo, JB); }
        J_SD(JT6, F_BM, JSP);
        jjal_code(s_jit_c[word ? JC_WR16 : JC_WR8]);
        J_NOP();
        { const int lo = jaddr(&s_interp_bus_master); J_LD(JT6, lo, JB); J_LD(JT7, F_BM, JSP);
          J_DSUBU(JT6, JT6, JT7); J_ADDU(JS4, JS4, JT6); J_SD(JT7, lo, JB); }
        J_LD(JRA, F_RA4, JSP);
        J_JR(JRA);
        J_NOP();
    }
    /* Shared read routines. In: a0 (and a1 for a word's high byte) = the
     * address, v1 = the instruction's last fetched byte and its address
     * << 8 (the bus latch a slow access must find). Out: v0. A page of plain
     * RAM or ROM is read here, as bridge_bus_read's fast path does it: the
     * access's speed onto s4, the value into the open-bus latch, the
     * address into the cartridge's. Anything else goes to the C functions,
     * a word as a word (hardware register pairs are claimed whole). They
     * use t6-t9 and at; a C call clobbers the rest of the temporaries. */
    for (unsigned pgi = 0; pgi < kCpuFastMapPages; pgi++)
        s_jit_speed[pgi] = (uint8_t)cpu_region_speed_inline((uint32_t)pgi << kCpuFastMapPageShift);
    for (int word = 0; word < 2; word++) {
        if (word) s_jit_rd16 = s_jit_n; else s_jit_rd8 = s_jit_n;
        const uint32_t slow_br[2] = { 0, 0 }; (void)slow_br;
        uint32_t br0, br1 = 0;
        J_SRL(JT8, JA0, kCpuFastMapPageShift);
        J_SLL(JT9, JT8, 2);
        { const int lo = jaddr_at(g_cpu_fast_read_page); J_ADDU(JAT, JAT, JT9); J_LW(JT9, lo, JAT); }
        br0 = s_jit_n; jI(4, JT9, JZERO, 0);                 /* beqz: not a plain page */
        if (!word) {
            J_ANDI(JT6, JA0, 0x1fff);
            J_ADDU(JT9, JT9, JT6);
            J_LBU(JV0, 0, JT9);
            { const int lo = jaddr_at(s_jit_speed); J_ADDU(JAT, JAT, JT8); J_LBU(JT8, lo, JAT); }
            J_ADDU(JS4, JS4, JT8);
            { const int lo = jaddr(&g_open_bus); J_SB(JV0, lo, JB); }
            { const int lo = jaddr(&g_snes); J_LW(JT9, lo, JB); }
            J_LW(JT9, (int)offsetof(Snes, cart), JT9);
            J_JR(JRA);
            J_SW(JA0, (int)offsetof(Cart, cpuBusAddress), JT9);
        } else {
            J_SRL(JT6, JA1, kCpuFastMapPageShift);
            J_SLL(JT7, JT6, 2);
            { const int lo = jaddr_at(g_cpu_fast_read_page); J_ADDU(JAT, JAT, JT7); J_LW(JT7, lo, JAT); }
            br1 = s_jit_n; jI(4, JT7, JZERO, 0);
            J_ANDI(JV0, JA0, 0x1fff);
            J_ADDU(JT9, JT9, JV0);
            J_LBU(JV0, 0, JT9);                              /* low byte */
            J_ANDI(JT9, JA1, 0x1fff);
            J_ADDU(JT7, JT7, JT9);
            J_LBU(JT7, 0, JT7);                              /* high byte */
            { const int lo = jaddr_at(s_jit_speed); J_ADDU(JT9, JAT, JT8); J_LBU(JT8, lo, JT9);
              J_ADDU(JT9, JAT, JT6); J_LBU(JT6, lo, JT9); }
            J_ADDU(JS4, JS4, JT8);
            J_ADDU(JS4, JS4, JT6);
            { const int lo = jaddr(&g_open_bus); J_SB(JT7, lo, JB); }
            J_SLL(JT7, JT7, 8);
            J_OR(JV0, JV0, JT7);
            { const int lo = jaddr(&g_snes); J_LW(JT9, lo, JB); }
            J_LW(JT9, (int)offsetof(Snes, cart), JT9);
            J_JR(JRA);
            J_SW(JA1, (int)offsetof(Cart, cpuBusAddress), JT9);
        }
        /* slow: the latch, then the C function; its accesses' speed is in
         * s_interp_bus_master. */
        s_jit_code[br0] |= (s_jit_n - br0 - 1) & 0xffffu;
        if (word) s_jit_code[br1] |= (s_jit_n - br1 - 1) & 0xffffu;
        J_SD(JRA, F_RA2, JSP);
        { const int lo = jaddr(&g_open_bus); J_SB(JV1, lo, JB); }
        J_SRL(JT6, JV1, 8);
        { const int lo = jaddr(&g_snes); J_LW(JT9, lo, JB); }
        J_LW(JT9, (int)offsetof(Snes, cart), JT9);
        J_SW(JT6, (int)offsetof(Cart, cpuBusAddress), JT9);
        { const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JB); }
        jit_syncout_body();
        jit_spill_axy();
        jjal(word ? (void *)jit_rd16 : (void *)jit_rd8);
        J_NOP();
        jit_syncin();
        { const int lo = jaddr(&s_interp_bus_master); J_LD(JT8, lo, JB); }
        J_ADDU(JS4, JS4, JT8);
        J_LD(JRA, F_RA2, JSP);
        J_JR(JRA);
        J_NOP();
    }
    s_jit_xcommon = s_jit_n;
    {
        const int lnext = jlabel(), lnolatch = jlabel();
        J_SLL(JT0, JT0, 2); J_ADDU(JT1, JT1, JT0); J_LW(JT3, 0, JT1);
        J_ANDI(JT4, JT3, 0xffff);                       /* the instruction's pc */
        J_SRL(JT6, JT3, 24); J_ANDI(JT6, JT6, 3);
        J_OR(JT7, JT2, JT4);
        { const int lo = jaddr(&g_jit816_prev); J_SW(JT7, lo, JB); }
        J_ADDU(JT7, JT4, JT6); J_ANDI(JT7, JT7, 0xffff);   /* its last byte's address */
        jbr(1, JT5, JZERO, lnext);                      /* bltz (regimm 0): the next instruction */
        J_ADDIU(JT8, JT7, 1);
        J_MOVE(JT8, JT5);
        jbind(lnext);
        { const int lo = jaddr(&s_jit_in); J_LW(JT9, lo, JB); }
        J_SH(JT8, (int)offsetof(Interp816, pc), JT9);
        J_SRL(JT9, JT3, 26); J_ANDI(JT9, JT9, 1);
        jbr(4, JT9, JZERO, lnolatch);
        J_NOP();
        J_SRL(JT6, JT3, 16);
        { const int lo = jaddr(&g_open_bus); J_SB(JT6, lo, JB); }
        { const int lo = jaddr(&g_snes); J_LW(JT6, lo, JB); }
        J_LW(JT6, (int)offsetof(Snes, cart), JT6);
        J_OR(JT7, JT2, JT7);
        J_SW(JT7, (int)offsetof(Cart, cpuBusAddress), JT6);
        jbind(lnolatch);
#ifdef N64_JIT_EXITS
        /* kind: t5 -1 -> 0 (the event), -2 -> 2 (the block's end), a pc -> 1 */
        { const int lpos = jlabel(), ldone = jlabel();
          jbr(1, JT5, JZERO, lpos);                     /* bltz */
          J_ORI(JT6, JZERO, 4);                         /* (a pc: index 1) */
          jbr(4, JZERO, JZERO, ldone);
          J_NOP();
          jbind(lpos);
          J_ADDIU(JT6, JT5, 1); J_SLL(JT6, JT6, 3); jR(JZERO, JT6, JT6, 0, 0x23);   /* -(t5 + 1) * 8: 0 or 8 */
          jbind(ldone);
          { const int lo = jaddr_at(g_jit816_exit); J_ADDU(JAT, JAT, JT6); J_LW(JT7, lo, JAT); J_ADDIU(JT7, JT7, 1); J_SW(JT7, lo, JAT); } }
#endif
        jit_spill_axy();
        jit_syncout_body();
        jj(s_jit_epilogue);
        J_NOP();
    }
    jfix_all();
#ifdef N64_JIT_DUMP
    { extern void n64_platform_log(const char *fmt, ...);
      static int once;
      if (!once++) {
        n64_platform_log("[jitshared] words %lu: syncout %lu c0 %lu st8 %lu st16 %lu rd8 %lu rd16 %lu xcommon %lu\n", (unsigned long)s_jit_n,
                         (unsigned long)s_jit_syncout, (unsigned long)s_jit_c[0], (unsigned long)s_jit_st8, (unsigned long)s_jit_st16,
                         (unsigned long)s_jit_rd8, (unsigned long)s_jit_rd16, (unsigned long)s_jit_xcommon);
        for (uint32_t w = 0; w < s_jit_n; w += 8) {
          char line[100]; int o = 0;
          for (uint32_t k = w; k < w + 8 && k < s_jit_n; k++) o += snprintf(line + o, sizeof line - (size_t)o, "%08lx ", (unsigned long)s_jit_code[k]);
          n64_platform_log("[jitshared] w %s\n", line);
        }
      } }
#endif
    data_cache_hit_writeback(&s_jit_code[0], s_jit_n * 4);
    inst_cache_hit_invalidate(&s_jit_code[0], s_jit_n * 4);
}

static JitEntry *jit_find(uint32_t key) {
    for (uint32_t h = (key ^ (key >> 11)) & (kJitEntries - 1);; h = (h + 1) & (kJitEntries - 1)) {
        JitEntry *e = &s_jit_ent[h];
        if (e->key == key) return e;
        if (!e->key) return NULL;
    }
}
static void jit_add(uint32_t key, uint32_t code, const uint8_t *pg, uint32_t n) {
    for (uint32_t h = (key ^ (key >> 11)) & (kJitEntries - 1);; h = (h + 1) & (kJitEntries - 1)) {
        JitEntry *e = &s_jit_ent[h];
        if (e->key == key) { e->code = code; e->pg = pg; e->n = n; return; }   /* (the newer block: it may hold a loop) */
        if (!e->key) { e->key = key; e->code = code; e->pg = pg; e->n = n; s_jit_nent++; return; }
    }
}

/* One translated instruction's description. */
enum { JM_NONE, JM_IMM, JM_DP, JM_ABS, JM_ABX, JM_ABY, JM_REL, JM_ALX, JM_ILY, JM_IMM8, JM_XABS, JM_XLONG };
enum { JO_NONE, JO_LDA, JO_LDX, JO_LDY, JO_STA, JO_STX, JO_STY, JO_STZ, JO_AND, JO_ORA, JO_EOR, JO_CMP,
       JO_CPX, JO_CPY, JO_INX, JO_INY, JO_DEX, JO_DEY, JO_TAX, JO_TAY, JO_TXA, JO_TYA, JO_CLC, JO_SEC,
       JO_BR, JO_BRA,
       JO_ADC, JO_SBC, JO_INC, JO_DEC,                       /* INC/DEC: memory */
       JO_ASLA, JO_LSRA, JO_ROLA, JO_RORA, JO_INCA, JO_DECA, JO_XBA,
       JO_SEP, JO_REP, JO_PHA, JO_PHX, JO_PHY, JO_PLA, JO_PLX, JO_PLY, JO_BIT,
       JO_JMP, JO_JML, JO_JSR, JO_JSL, JO_RTS, JO_RTL };   /* transfers: they end a block */
typedef struct { uint8_t op, mode; } JitOp;
static JitOp jit_decode(uint8_t opc) {
    static const struct { uint8_t opc, op, mode; } k[] = {
        {0xa9, JO_LDA, JM_IMM}, {0xa5, JO_LDA, JM_DP}, {0xad, JO_LDA, JM_ABS}, {0xbd, JO_LDA, JM_ABX}, {0xb9, JO_LDA, JM_ABY},
        {0xa2, JO_LDX, JM_IMM}, {0xa6, JO_LDX, JM_DP}, {0xae, JO_LDX, JM_ABS}, {0xbe, JO_LDX, JM_ABY},
        {0xa0, JO_LDY, JM_IMM}, {0xa4, JO_LDY, JM_DP}, {0xac, JO_LDY, JM_ABS}, {0xbc, JO_LDY, JM_ABX},
        {0x85, JO_STA, JM_DP}, {0x8d, JO_STA, JM_ABS}, {0x9d, JO_STA, JM_ABX}, {0x99, JO_STA, JM_ABY},
        {0x86, JO_STX, JM_DP}, {0x8e, JO_STX, JM_ABS}, {0x84, JO_STY, JM_DP}, {0x8c, JO_STY, JM_ABS},
        {0x64, JO_STZ, JM_DP}, {0x9c, JO_STZ, JM_ABS}, {0x9e, JO_STZ, JM_ABX},
        {0x29, JO_AND, JM_IMM}, {0x25, JO_AND, JM_DP}, {0x2d, JO_AND, JM_ABS}, {0x3d, JO_AND, JM_ABX}, {0x39, JO_AND, JM_ABY},
        {0x09, JO_ORA, JM_IMM}, {0x05, JO_ORA, JM_DP}, {0x0d, JO_ORA, JM_ABS}, {0x1d, JO_ORA, JM_ABX}, {0x19, JO_ORA, JM_ABY},
        {0x49, JO_EOR, JM_IMM}, {0x45, JO_EOR, JM_DP}, {0x4d, JO_EOR, JM_ABS}, {0x5d, JO_EOR, JM_ABX}, {0x59, JO_EOR, JM_ABY},
        {0xc9, JO_CMP, JM_IMM}, {0xc5, JO_CMP, JM_DP}, {0xcd, JO_CMP, JM_ABS}, {0xdd, JO_CMP, JM_ABX}, {0xd9, JO_CMP, JM_ABY},
        {0xe0, JO_CPX, JM_IMM}, {0xe4, JO_CPX, JM_DP}, {0xec, JO_CPX, JM_ABS},
        {0xc0, JO_CPY, JM_IMM}, {0xc4, JO_CPY, JM_DP}, {0xcc, JO_CPY, JM_ABS},
        {0xe8, JO_INX, JM_NONE}, {0xc8, JO_INY, JM_NONE}, {0xca, JO_DEX, JM_NONE}, {0x88, JO_DEY, JM_NONE},
        {0xaa, JO_TAX, JM_NONE}, {0xa8, JO_TAY, JM_NONE}, {0x8a, JO_TXA, JM_NONE}, {0x98, JO_TYA, JM_NONE},
        {0x18, JO_CLC, JM_NONE}, {0x38, JO_SEC, JM_NONE},
        {0x10, JO_BR, JM_REL}, {0x30, JO_BR, JM_REL}, {0x50, JO_BR, JM_REL}, {0x70, JO_BR, JM_REL},
        {0x90, JO_BR, JM_REL}, {0xb0, JO_BR, JM_REL}, {0xd0, JO_BR, JM_REL}, {0xf0, JO_BR, JM_REL},
        {0x80, JO_BRA, JM_REL},
        {0x69, JO_ADC, JM_IMM}, {0x65, JO_ADC, JM_DP}, {0x6d, JO_ADC, JM_ABS}, {0x7d, JO_ADC, JM_ABX}, {0x79, JO_ADC, JM_ABY},
        {0x7f, JO_ADC, JM_ALX}, {0x77, JO_ADC, JM_ILY},
        {0xe9, JO_SBC, JM_IMM}, {0xe5, JO_SBC, JM_DP}, {0xed, JO_SBC, JM_ABS}, {0xfd, JO_SBC, JM_ABX}, {0xf9, JO_SBC, JM_ABY},
        {0xff, JO_SBC, JM_ALX}, {0xf7, JO_SBC, JM_ILY},
        {0xbf, JO_LDA, JM_ALX}, {0xb7, JO_LDA, JM_ILY}, {0x9f, JO_STA, JM_ALX}, {0x97, JO_STA, JM_ILY},
        {0xdf, JO_CMP, JM_ALX}, {0xd7, JO_CMP, JM_ILY}, {0x3f, JO_AND, JM_ALX}, {0x37, JO_AND, JM_ILY},
        {0x1f, JO_ORA, JM_ALX}, {0x17, JO_ORA, JM_ILY}, {0x5f, JO_EOR, JM_ALX}, {0x57, JO_EOR, JM_ILY},
        {0xe6, JO_INC, JM_DP}, {0xee, JO_INC, JM_ABS}, {0xfe, JO_INC, JM_ABX},
        {0xc6, JO_DEC, JM_DP}, {0xce, JO_DEC, JM_ABS}, {0xde, JO_DEC, JM_ABX},
        {0x0a, JO_ASLA, JM_NONE}, {0x4a, JO_LSRA, JM_NONE}, {0x2a, JO_ROLA, JM_NONE}, {0x6a, JO_RORA, JM_NONE},
        {0x1a, JO_INCA, JM_NONE}, {0x3a, JO_DECA, JM_NONE}, {0xeb, JO_XBA, JM_NONE},
        {0xe2, JO_SEP, JM_IMM8}, {0xc2, JO_REP, JM_IMM8},
        {0x89, JO_BIT, JM_IMM}, {0x24, JO_BIT, JM_DP}, {0x2c, JO_BIT, JM_ABS}, {0x3c, JO_BIT, JM_ABX},
        {0x48, JO_PHA, JM_NONE}, {0xda, JO_PHX, JM_NONE}, {0x5a, JO_PHY, JM_NONE},
        {0x68, JO_PLA, JM_NONE}, {0xfa, JO_PLX, JM_NONE}, {0x7a, JO_PLY, JM_NONE},
#if SNESRECOMP_JIT_XFER
        {0x4c, JO_JMP, JM_XABS}, {0x5c, JO_JML, JM_XLONG}, {0x20, JO_JSR, JM_XABS}, {0x22, JO_JSL, JM_XLONG},
        {0x60, JO_RTS, JM_NONE}, {0x6b, JO_RTL, JM_NONE},
#endif
    };
    /* (by opcode, filled on first use: the lean loop asks for every
     * opcode it would otherwise stop at) */
    static JitOp tab[256];
    static bool ready;
    if (!ready) {
        for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) { tab[k[i].opc].op = k[i].op; tab[k[i].opc].mode = k[i].mode; }
        ready = true;
    }
    return tab[opc];
}
static bool jit_op_x(int op) {   /* width from X (index registers) rather than M */
    return op == JO_LDX || op == JO_LDY || op == JO_STX || op == JO_STY || op == JO_CPX || op == JO_CPY ||
           op == JO_PHX || op == JO_PHY || op == JO_PLX || op == JO_PLY;
}
static bool jit_op_xfer(int op) { return op >= JO_JMP && op <= JO_RTL; }
/* A direct transfer's target, and whether a block may make it: the target is
 * ROM (so what is known of it now holds) and has no compiled body -- the
 * big loop runs a compiled body in place of a call it interprets, and a jump
 * to one under a recorded call's return. */
static uint32_t jit_xfer_target(uint32_t pc24, const uint8_t *pg, JitOp o) {
    const uint32_t pc = pc24 & 0xFFFFu;
    const uint32_t t16 = pg[(pc + 1) & 0x1FFFu] | (uint32_t)pg[(pc + 2) & 0x1FFFu] << 8;
    return (o.mode == JM_XLONG ? (uint32_t)pg[(pc + 3) & 0x1FFFu] << 16 : (pc24 & 0xFF0000u)) | t16;
}
static bool jit_xfer_ok(uint32_t target) {
    const uint32_t bank = target >> 16;
    if (bank == 0x7E || bank == 0x7F || (target & 0xFFFFu) < 0x8000u) return false;
    return !cpu_dispatch_has_entry(s_jit_cpu, target);
}
static bool jit_op_store(int op) { return op == JO_STA || op == JO_STX || op == JO_STY || op == JO_STZ; }

/* nz = the lazy N/Z register (cpu_regs.h): an 8-bit result in bits 8-15, a
 * 16-bit one as it is. r holds the result (any high bits); uses JT5. */
static void jit_set_nz(int r, bool byte) {
    if (byte) { J_ANDI(JT5, r, 0xff); J_SLL(JT5, JT5, 8); }
    else J_ANDI(JT5, r, 0xffff);
    J_MTC1(JT5, FNZ);
}
/* The traces of the last opcode/operand byte's read: the open-bus latch and
 * the cartridge's bus address. */
static void jit_latch(uint32_t adr, uint8_t byte) {
    int lo = jaddr(&g_open_bus);
    J_ORI(JT5, JZERO, byte);
    J_SB(JT5, lo, JB);
    lo = jaddr(&g_snes);
    J_LW(JT5, lo, JB);
    J_LW(JT5, (int)offsetof(Snes, cart), JT5);
    jli(JAT, adr & 0xFFFFFFu);
    J_SW(JAT, (int)offsetof(Cart, cpuBusAddress), JT5);
}
/* Leave the block: pc, the last instruction's pc for the caller. */
#ifdef N64_JIT_EXITS
/* Why blocks are left, counted at run time: 0 the event, 1 a taken branch
 * out of the block (or a loop out of laps), 2 the block's end; and for the
 * end, the opcode found there. */
uint32_t g_jit816_exit[3], g_jit816_exit_op[256];
static int s_jit_exit_kind, s_jit_exit_op;
#define JIT_EXIT_KIND(k, op) (s_jit_exit_kind = (k), s_jit_exit_op = (op))
#else
#define JIT_EXIT_KIND(k, op) ((void)0)
#endif
static void jit_exit(uint16_t pc, uint32_t last_pc24) {
    jjal_code(s_jit_syncout); J_NOP();
#ifdef N64_JIT_EXITS
    { int lo = jaddr(&g_jit816_exit[s_jit_exit_kind]); J_LW(JT0, lo, JB); J_ADDIU(JT0, JT0, 1); J_SW(JT0, lo, JB);
      if (s_jit_exit_kind == 2) { lo = jaddr(&g_jit816_exit_op[s_jit_exit_op]); J_LW(JT0, lo, JB); J_ADDIU(JT0, JT0, 1); J_SW(JT0, lo, JB); } }
#endif
    jit_spill_axy();
    jli(JT0, pc);
    { const int lo = jaddr(&s_jit_in); J_LW(JT1, lo, JB); }
    J_SH(JT0, (int)offsetof(Interp816, pc), JT1);
    jli(JT0, last_pc24);
    const int lo = jaddr(&g_jit816_prev);
    jj(s_jit_epilogue);
    J_SW(JT0, lo, JB);
}

/* The instructions of a block that starts at pc24: their addresses (and the
 * address after the last), lengths and operations. count: note the opcode
 * that ended it. */
static int jit_scan(uint32_t pc24, const uint8_t *pg, bool mf, bool xf, bool count,
                    uint16_t *ipc, uint8_t *ilen, JitOp *iop) {
    const uint32_t bank = pc24 & 0xFF0000u;
    int n = 0;
    uint32_t pc = pc24 & 0xFFFFu;
    const uint32_t page_end = (pc | 0x1FFFu) + 1;
    while (n < kJitMaxInsns) {
        const uint8_t opc = pg[pc & 0x1FFFu];
        const JitOp o = jit_decode(opc);
        if (o.op == JO_NONE || (k_burst_stop[opc] && !jit_op_xfer(o.op))) { if (count) g_jit816_ended[opc]++; break; }
        /* A return is a block of its own: it may have to leave before it
         * does anything (s_jit_ret_limit), which only a block's first
         * instruction can (nothing of an instruction before it is pending). */
        if ((o.op == JO_RTS || o.op == JO_RTL) && n) break;
        if ((o.mode == JM_XABS || o.mode == JM_XLONG) && !jit_xfer_ok(jit_xfer_target(bank | pc, pg, o))) {
            if (count) g_jit816_ended[opc]++;
            break;
        }
        const bool byte = jit_op_x(o.op) ? xf : mf;
        const int len = o.mode == JM_NONE ? 1 : o.mode == JM_IMM ? (byte ? 2 : 3) :
                        (o.mode == JM_DP || o.mode == JM_REL || o.mode == JM_ILY || o.mode == JM_IMM8) ? 2 :
                        (o.mode == JM_ALX || o.mode == JM_XLONG) ? 4 : 3;
        if (pc + (uint32_t)len > page_end) break;
        /* SEP and REP of N or Z have no translation (they go through the
         * whole status byte). */
        if ((o.op == JO_SEP || o.op == JO_REP) && (pg[(pc + 1) & 0x1FFFu] & 0x82u)) { if (count) g_jit816_ended[opc]++; break; }
#ifdef N64_STATE_AT_PC
        if ((bank | pc) == (uint32_t)(N64_STATE_AT_PC)) break;
#endif
        ipc[n] = (uint16_t)pc; ilen[n] = (uint8_t)len; iop[n] = o;
        n++;
        pc += (uint32_t)len;
        if (o.op == JO_BRA || o.op == JO_SEP || o.op == JO_REP || jit_op_xfer(o.op)) break;   /* (the widths may change: the block ends) */
    }
    ipc[n] = (uint16_t)pc;
    return n;
}

/* Translate the block that starts at pc24 (page pg, widths mf/xf). Returns
 * the number of instructions translated (0: none). */
static int jit_translate(uint32_t pc24, const uint8_t *pg, bool mf, bool xf) {
    const uint32_t mx = (mf ? 2u : 0u) | (xf ? 1u : 0u);
    const uint32_t bank = pc24 & 0xFF0000u;
    uint16_t ipc[kJitMaxInsns + 1];
    uint8_t ilen[kJitMaxInsns];
    JitOp iop[kJitMaxInsns];
    int n = jit_scan(pc24, pg, mf, xf, true, ipc, ilen, iop);
    if (!n) return 0;
    /* A loop reached past its head (the big loop ran the head): if a branch
     * in the block goes back to just before it, start the block there, so
     * that the loop is inside it. The block must still contain pc24. */
    for (int i = 0; i < n; i++) {
        if (iop[i].mode != JM_REL) continue;
        const uint16_t target = (uint16_t)(ipc[i] + 2 + (int8_t)pg[(ipc[i] + 1) & 0x1FFFu]);
        if (target >= ipc[0] || ipc[0] - target > 96 || ((target ^ ipc[0]) & 0xE000u)) continue;
        uint16_t ipc2[kJitMaxInsns + 1];
        uint8_t ilen2[kJitMaxInsns];
        JitOp iop2[kJitMaxInsns];
        const int n2 = jit_scan(bank | target, pg, mf, xf, false, ipc2, ilen2, iop2);
        bool has_start = false, has_branch = false;
        for (int j = 0; j < n2; j++) {
            if (ipc2[j] == ipc[0]) has_start = true;
            if (ipc2[j] == ipc[i]) has_branch = true;
        }
        if (has_start && has_branch) {
            n = n2;
            memcpy(ipc, ipc2, sizeof ipc); memcpy(ilen, ilen2, sizeof ilen); memcpy(iop, iop2, sizeof iop);
        }
        break;
    }

    s_jit_nlabels = 0; s_jit_nfix = 0;
    s_jit_cache = SNESRECOMP_JIT_AXY_REGS != 0;
    int label[kJitMaxInsns + 1], lexit[kJitMaxInsns];
    for (int i = 0; i <= n; i++) label[i] = jlabel();
    const int lthunk_next = jlabel(), lthunk_to = jlabel();
    uint32_t tab[kJitMaxInsns];
    memset(tab, 0, sizeof tab);
    uint32_t at[kJitMaxInsns];

    for (int i = 0; i < n; i++) {
        const uint32_t ia = bank | ipc[i];
        const uint8_t opc = pg[ipc[i] & 0x1FFFu];
        const uint8_t b1 = ilen[i] > 1 ? pg[(ipc[i] + 1) & 0x1FFFu] : 0;
        const uint8_t b2 = ilen[i] > 2 ? pg[(ipc[i] + 2) & 0x1FFFu] : 0;
        const JitOp o = iop[i];
        const bool byte = jit_op_x(o.op) ? xf : mf;
        const uint32_t m = byte ? 0xffu : 0xffffu;
        jbind(label[i]);
        at[i] = s_jit_n;
        lexit[i] = jlabel();
        const uint32_t w_start = s_jit_n;

        /* Clocks known now: every fetched byte's speed (an immediate's bytes
         * are fetched bytes), then 6 per internal cycle. accesses = bus
         * accesses in all, to find the internal cycles from the cycle count. */
        uint32_t fetch = 0;
        for (int b = 0; b < ilen[i]; b++) fetch += cpu_region_speed_inline(bank | ((ipc[i] + (uint32_t)b) & 0xFFFFu));
        const uint8_t b3 = ilen[i] > 3 ? pg[(ipc[i] + 3) & 0x1FFFu] : 0;
        const bool data = o.mode == JM_DP || o.mode == JM_ABS || o.mode == JM_ABX || o.mode == JM_ABY ||
                          o.mode == JM_ALX || o.mode == JM_ILY;
        const bool rmw = o.op == JO_INC || o.op == JO_DEC;
        const bool push = o.op == JO_PHA || o.op == JO_PHX || o.op == JO_PHY;
        const bool pull = o.op == JO_PLA || o.op == JO_PLX || o.op == JO_PLY;
        const bool bus = data || push || pull;   /* makes bus accesses at run time (they leave the bus latch) */
        int cyc = cyclesPerOpcode[opc];
        int accesses = ilen[i];
        if (rmw) {
            if (!byte) cyc += 2;                  /* 16-bit: 2 extra cycles */
            accesses += byte ? 2 : 4;
        } else if (bus || o.mode == JM_IMM) {
            if (!byte) cyc++;                     /* 16-bit: 1 extra cycle */
            if (bus) accesses += byte ? 1 : 2;
        }
        if (o.mode == JM_ILY) accesses += 3;      /* the pointer */
        if (o.op == JO_JSR || o.op == JO_RTS) accesses += 2;   /* the return address */
        if (o.op == JO_JSL || o.op == JO_RTL) accesses += 3;
        const int internal = cyc > accesses ? cyc - accesses : 0;
        const uint32_t fixed = fetch + (uint32_t)internal * 6u;
        const uint32_t last_adr = bank | ((ipc[i] + ilen[i] - 1u) & 0xFFFFu);
        const uint8_t last_byte = pg[(ipc[i] + ilen[i] - 1u) & 0x1FFFu];

        if (o.op == JO_BR || o.op == JO_BRA) {
            /* The flag, taken when it equals bit 5 of the opcode. */
            const uint16_t target = (uint16_t)(ipc[i] + 2 + (int8_t)b1);
            int tl = -1;
            for (int j = 0; j < n; j++) if (ipc[j] == target) tl = j;
            const int ltaken = jlabel();
            if (o.op == JO_BR) {
                const int sel = opc >> 6, want = (opc >> 5) & 1;
                if (sel == 0) { J_MFC1(JT0, FNZ); J_SRL(JT0, JT0, 15); J_SLTU(JT0, JZERO, JT0); }
                else if (sel == 1) { J_MFC1(JT0, FV); }
                else if (sel == 2) { J_MFC1(JT0, FC); }
                else { J_MFC1(JT0, FNZ); J_ANDI(JT0, JT0, 0xffff); J_SLTIU(JT0, JT0, 1); }
                jbr(want ? 5 : 4, JT0, JZERO, ltaken);
                J_ORI(JS4, JZERO, (int)fixed);      /* (delay slot) not taken: 2 cycles */
                /* not taken */
                J_DSUBU(JR, JR, JS4);
                J_ADDIU(JS1, JS1, 1);
                jbr(6, JR, JZERO, lthunk_next); J_ORI(JT0, JZERO, i);   /* blez: the event */
                { const int lskip = label[i + 1]; jbr(4, JZERO, JZERO, lskip); J_NOP(); }
            }
            /* taken: one more internal cycle (BRA's is in its count) */
            jbind(ltaken);
            J_ORI(JS4, JZERO, (int)(fixed + (o.op == JO_BR ? 6u : 0u)));
            J_DSUBU(JR, JR, JS4);
            J_ADDIU(JS1, JS1, 1);
            if (tl >= 0) {
                J_SLT(JT4, JZERO, JR);                    /* time left */
                if (tl <= i) {
                    /* A loop inside the block. While it makes progress --
                     * A, X or Y differ from the lap before, or something
                     * was written -- it stays. A lap that changed none of
                     * them may be a wait for an interrupt: after a few of
                     * those in a row it leaves by the branch, so that the
                     * big loop's wait detector sees the loop's head. (And
                     * it leaves after kJitRunMax instructions in any case:
                     * the caller's step cap.) */
                    J_MFC1(JT0, FA); J_MFC1(JT1, FX); J_MFC1(JT2, FY);
                    J_SLL(JT1, JT1, 8); J_XOR(JT0, JT0, JT1);
                    J_SLL(JT2, JT2, 16); J_XOR(JT0, JT0, JT2);
                    { const int lo = jaddr((const uint8_t *)&g_interp_bridge_write_epoch + sizeof g_interp_bridge_write_epoch - 4);
                      J_LW(JT1, lo, JB); }
                    J_SLL(JT2, JT1, 24); J_XOR(JT0, JT0, JT2); J_XOR(JT0, JT0, JT1);
                    { const int lo = jaddr(&s_jit_fp); J_LW(JT1, lo, JB); J_SW(JT0, lo, JB); }
                    J_XOR(JT1, JT1, JT0);
                    J_LD(JT3, F_LAPS, JSP);
                    jI(4, JT1, JZERO, 2);                 /* the same as last lap: one fewer left */
                    J_ADDIU(JT3, JT3, -1);
                    J_ORI(JT3, JZERO, kJitLaps);          /* progress: the full allowance again */
                    J_SD(JT3, F_LAPS, JSP);
                    J_SLTU(JT5, JZERO, JT3);
                    J_AND(JT4, JT4, JT5);
                    J_SLTIU(JT5, JS1, kJitRunMax);
                    J_AND(JT4, JT4, JT5);
                }
                jbr(5, JT4, JZERO, label[tl]); J_NOP();
            }
            /* out of the block, to the target */
            J_ORI(JT5, JZERO, target);
            jbr(4, JZERO, JZERO, lthunk_to); J_ORI(JT0, JZERO, i);
            jbind(lexit[i]);
            tab[i] = (uint32_t)ipc[i] | (uint32_t)last_byte << 16 | (uint32_t)(ilen[i] - 1) << 24 | 1u << 26;
            g_jit816_words[3] += s_jit_n - w_start;
            continue;
        }

        if (jit_op_xfer(o.op)) {
            const bool call = o.op == JO_JSR || o.op == JO_JSL, ret = o.op == JO_RTS || o.op == JO_RTL;
            const bool lng = o.op == JO_JSL || o.op == JO_RTL || o.op == JO_JML;
            const uint32_t target = ret ? 0 : jit_xfer_target(ia, pg, o);
            if (ret) {
                /* Leaving S above the run's entry depth is the run's end:
                 * the big loop's to do. (i == 0: jit_scan.) */
                J_MFC1(JS2, FS);
                J_ADDIU(JT0, JS2, lng ? 3 : 2); J_ANDI(JT0, JT0, 0xffff);
                { const int lo = jaddr(&s_jit_ret_limit); J_LW(JT1, lo, JB); }
                J_SLTU(JT2, JT1, JT0);
                jbr(5, JT2, JZERO, lexit[i]); J_NOP();
            }
            J_ORI(JS4, JZERO, (int)fixed);
            if (call) {
                /* pushByte(K) for a long call, then pushWord(the last operand byte's address) */
                J_MFC1(JS2, FS);
                if (lng) {
                    J_MFC1(JA1, FPB); jli(JV1, (last_adr & 0xFFFFFFu) << 8 | last_byte); jli(JT5, ia); jjal_code(s_jit_st8); J_MOVE(JA0, JS2);
                    J_ADDIU(JS2, JS2, -1); J_ANDI(JS2, JS2, 0xffff);
                }
                const uint32_t r16 = (ipc[i] + ilen[i] - 1u) & 0xFFFFu;
                J_ORI(JA1, JZERO, (int)(r16 >> 8));
                if (lng) { J_MOVE(JV1, JZERO); J_MOVE(JT5, JZERO); } else { jli(JV1, (last_adr & 0xFFFFFFu) << 8 | last_byte); jli(JT5, ia); }
                jjal_code(s_jit_st8); J_MOVE(JA0, JS2);
                J_ADDIU(JS2, JS2, -1); J_ANDI(JS2, JS2, 0xffff);
                J_ORI(JA1, JZERO, (int)(r16 & 0xff)); J_MOVE(JV1, JZERO); J_MOVE(JT5, JZERO); jjal_code(s_jit_st8); J_MOVE(JA0, JS2);
                J_ADDIU(JS2, JS2, -1); J_ANDI(JS2, JS2, 0xffff);
                J_MTC1(JS2, FS);
                /* the call's record, as the big loop makes it for a call it interprets */
                jli(JA2, (ia + ilen[i]) & 0xFFFFFFu);
                J_ORI(JA1, JZERO, cpu_dispatch_inline_arg_bytes(target) ? 0 : (lng ? 3 : 2));
                jjal(jit_tail_call); J_MOVE(JA0, JS2);
            } else if (ret) {
                J_ORI(JA1, JZERO, lng ? 3 : 2);
                jjal(jit_tail_ret); J_MOVE(JA0, JS2);
                /* pullWord + 1 (two byte reads), then pullByte for K */
                J_ADDIU(JS2, JS2, 1); J_ANDI(JS2, JS2, 0xffff);
                jit_read(true, JS2, 0, last_adr, last_byte);
                J_MOVE(JS3, JV0);
                J_ADDIU(JS2, JS2, 1); J_ANDI(JS2, JS2, 0xffff);
                { const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JB); }
                jjal_code(s_jit_c[JC_RD8]); J_MOVE(JA0, JS2);
                J_SLL(JV0, JV0, 8); J_OR(JS3, JS3, JV0);
                J_ADDIU(JS3, JS3, 1); J_ANDI(JS3, JS3, 0xffff);
                if (lng) {
                    J_ADDIU(JS2, JS2, 1); J_ANDI(JS2, JS2, 0xffff);
                    jjal_code(s_jit_c[JC_RD8]); J_MOVE(JA0, JS2);
                    J_ANDI(JT4, JV0, 0xff);
                    J_MTC1(JT4, FPB);
                }
                { const int lo = jaddr(&s_interp_bus_master); J_LD(JT0, lo, JB); J_ADDU(JS4, JS4, JT0); }
                J_MTC1(JS2, FS);
            } else jit_latch(last_adr, last_byte);
            if (!ret && lng) { J_ORI(JT0, JZERO, (int)(target >> 16)); J_MTC1(JT0, FPB); }
            J_DSUBU(JR, JR, JS4);
            J_ADDIU(JS1, JS1, 1);
            JIT_EXIT_KIND(1, 0);
            /* The lean loop's "last pc": for a call or a return, just below
             * the target (they are not loop heads: the loop goes on there);
             * for a jump, the jump (back is a loop head, as a branch). */
            if (ret) {
#ifdef N64_JIT_EXITS
                { const int lo = jaddr(&g_jit816_exit[1]); J_LW(JT0, lo, JB); J_ADDIU(JT0, JT0, 1); J_SW(JT0, lo, JB); }
#endif
                jjal_code(s_jit_syncout); J_NOP();
                jit_spill_axy();
                { const int lo = jaddr(&s_jit_in); J_LW(JT1, lo, JB); }
                J_SH(JS3, (int)offsetof(Interp816, pc), JT1);
                J_ADDIU(JT0, JS3, -1); J_ANDI(JT0, JT0, 0xffff);
                if (lng) J_SLL(JT1, JT4, 16); else jli(JT1, bank);
                J_OR(JT0, JT0, JT1);
                const int lo = jaddr(&g_jit816_prev);
                jj(s_jit_epilogue);
                J_SW(JT0, lo, JB);
            } else {
                jit_exit((uint16_t)target, call ? ((target & 0xFF0000u) | ((target - 1u) & 0xFFFFu)) : ia);
            }
            /* a return the big loop must make: nothing done, nothing run */
            jbind(lexit[i]);
            if (ret) { JIT_EXIT_KIND(2, opc); jit_exit(ipc[i], ia); }
            g_jit816_words[4] += s_jit_n - w_start;
            continue;
        }

        J_ORI(JS4, JZERO, (int)fixed);
        if (o.mode == JM_NONE) {
            switch (o.op) {
            case JO_INX: case JO_DEX: case JO_INY: case JO_DEY: {
                const int f = (o.op == JO_INX || o.op == JO_DEX) ? FX : FY;
                if (s_jit_cache) {
                const int r = jit_creg(f);
                J_ADDIU(r, r, (o.op == JO_INX || o.op == JO_INY) ? 1 : -1);
                J_ANDI(r, r, xf ? 0xff : 0xffff);
                jit_set_nz(r, xf);
                } else {
                J_MFC1(JT0, f);
                J_ADDIU(JT0, JT0, (o.op == JO_INX || o.op == JO_INY) ? 1 : -1);
                J_ANDI(JT0, JT0, xf ? 0xff : 0xffff);
                J_MTC1(JT0, f);
                jit_set_nz(JT0, xf);
                }
                break; }
            case JO_TAX: case JO_TAY:
                if (s_jit_cache) {
                const int r = o.op == JO_TAX ? JCX : JCY;
                if (xf) J_ANDI(r, JCA, 0xff); else J_MOVE(r, JCA);
                jit_set_nz(r, xf);
                } else {
                J_MFC1(JT0, FA);
                if (xf) J_ANDI(JT0, JT0, 0xff);
                J_MTC1(JT0, o.op == JO_TAX ? FX : FY);
                jit_set_nz(JT0, xf);
                }
                break;
            case JO_TXA: case JO_TYA:
                if (s_jit_cache) {
                const int r = o.op == JO_TXA ? JCX : JCY;
                if (mf) { J_ANDI(JT0, r, 0xff); J_ANDI(JCA, JCA, 0xff00); J_OR(JCA, JCA, JT0); }
                else J_MOVE(JCA, r);
                jit_set_nz(JCA, mf);
                } else {
                J_MFC1(JT0, o.op == JO_TXA ? FX : FY);
                if (mf) {
                    J_MFC1(JT1, FA);
                    J_ANDI(JT1, JT1, 0xff00); J_ANDI(JT0, JT0, 0xff); J_OR(JT0, JT0, JT1);
                }
                J_MTC1(JT0, FA);
                jit_set_nz(JT0, mf);
                }
                break;
            case JO_CLC: J_MTC1(JZERO, FC); break;
            case JO_SEC: J_ORI(JT0, JZERO, 1); J_MTC1(JT0, FC); break;
            case JO_ASLA: case JO_LSRA: case JO_ROLA: case JO_RORA: case JO_INCA: case JO_DECA: {
                /* T0 = A, T1 = the new value (its low 8 or 16 bits count) */
                J_MFC1(JT0, FA);
                if (o.op == JO_ASLA) {
                    J_SRL(JT2, JT0, mf ? 7 : 15); J_ANDI(JT2, JT2, 1); J_MTC1(JT2, FC);
                    J_SLL(JT1, JT0, 1);
                } else if (o.op == JO_LSRA) {
                    J_ANDI(JT2, JT0, 1); J_MTC1(JT2, FC);
                    if (mf) { J_SRL(JT1, JT0, 1); J_ANDI(JT1, JT1, 0x7f); } else J_SRL(JT1, JT0, 1);
                } else if (o.op == JO_ROLA) {
                    J_MFC1(JT2, FC);
                    J_SLL(JT1, JT0, 1); J_OR(JT1, JT1, JT2);
                    J_SRL(JT2, JT1, mf ? 8 : 16); J_ANDI(JT2, JT2, 1); J_MTC1(JT2, FC);
                } else if (o.op == JO_RORA) {
                    J_MFC1(JT2, FC);
                    J_SLL(JT2, JT2, mf ? 7 : 15);
                    if (mf) { J_SRL(JT1, JT0, 1); J_ANDI(JT1, JT1, 0x7f); } else J_SRL(JT1, JT0, 1);
                    J_OR(JT1, JT1, JT2);
                    J_ANDI(JT2, JT0, 1); J_MTC1(JT2, FC);
                } else {
                    J_ADDIU(JT1, JT0, o.op == JO_INCA ? 1 : -1);
                }
                if (mf) { J_ANDI(JT0, JT0, 0xff00); J_ANDI(JT1, JT1, 0xff); J_OR(JT1, JT1, JT0); }
                else J_ANDI(JT1, JT1, 0xffff);
                J_MTC1(JT1, FA);
                jit_set_nz(JT1, mf);
                break; }
            case JO_XBA:
                J_MFC1(JT0, FA);
                J_SRL(JT1, JT0, 8); J_ANDI(JT1, JT1, 0xff);      /* high */
                J_ANDI(JT0, JT0, 0xff); J_SLL(JT0, JT0, 8); J_OR(JT0, JT0, JT1);
                J_MTC1(JT0, FA);
                jit_set_nz(JT1, true);
                break;
            case JO_PHA: case JO_PHX: case JO_PHY: {
                const int f = o.op == JO_PHA ? FA : o.op == JO_PHX ? FX : FY;
                J_MFC1(JS2, FS);
                J_MFC1(JS3, f);
                if (byte) {
                    J_MOVE(JA1, JS3); jli(JV1, (last_adr & 0xFFFFFFu) << 8 | last_byte); jli(JT5, ia); jjal_code(s_jit_st8); J_MOVE(JA0, JS2);
                    J_ADDIU(JS2, JS2, -1);
                } else {                                         /* high byte, then low */
                    J_SRL(JA1, JS3, 8); jli(JV1, (last_adr & 0xFFFFFFu) << 8 | last_byte); jli(JT5, ia); jjal_code(s_jit_st8); J_MOVE(JA0, JS2);
                    J_ADDIU(JS2, JS2, -1); J_ANDI(JS2, JS2, 0xffff);
                    J_MOVE(JA1, JS3); J_MOVE(JV1, JZERO); J_MOVE(JT5, JZERO); jjal_code(s_jit_st8); J_MOVE(JA0, JS2);
                    J_ADDIU(JS2, JS2, -1);
                }
                J_ANDI(JS2, JS2, 0xffff);
                J_MTC1(JS2, FS);
                break; }
            case JO_PLA: case JO_PLX: case JO_PLY: {
                const int f = o.op == JO_PLA ? FA : o.op == JO_PLX ? FX : FY;
                J_MFC1(JS2, FS);
                J_ADDIU(JS2, JS2, 1); J_ANDI(JS2, JS2, 0xffff);
                jit_read(true, JS2, 0, last_adr, last_byte);
                J_MOVE(JS3, JV0);
                if (!byte) {                                     /* low byte, then high (two byte reads, as pullWord) */
                    J_ADDIU(JS2, JS2, 1); J_ANDI(JS2, JS2, 0xffff);
                    /* (through the C function: the bus latch is the first read's now, not the operand's) */
                    { const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JB); }
                    jjal_code(s_jit_c[JC_RD8]); J_MOVE(JA0, JS2);
                    { const int lo = jaddr(&s_interp_bus_master); J_LD(JT0, lo, JB); J_ADDU(JS4, JS4, JT0); }
                    J_SLL(JV0, JV0, 8); J_OR(JS3, JS3, JV0);
                }
                J_MTC1(JS2, FS);
                if (byte && o.op == JO_PLA) {                    /* 8-bit A keeps its high byte */
                    if (s_jit_cache) {
                    J_ANDI(JCA, JCA, 0xff00); J_OR(JCA, JCA, JS3);
                    } else {
                    J_MFC1(JT0, FA); J_ANDI(JT0, JT0, 0xff00); J_OR(JT0, JT0, JS3); J_MTC1(JT0, FA);
                    }
                } else J_MTC1(JS3, f);
                jit_set_nz(JS3, byte);
                break; }
            default: break;
            }
        } else if (o.mode == JM_IMM8) {
            /* SEP, REP of C, V and the M, X, D, I bits (scan refuses N and Z). */
            const bool set = o.op == JO_SEP;
            if (b1 & 0x01) { if (set) { J_ORI(JT0, JZERO, 1); J_MTC1(JT0, FC); } else J_MTC1(JZERO, FC); }
            if (b1 & 0x40) { if (set) { J_ORI(JT0, JZERO, 1); J_MTC1(JT0, FV); } else J_MTC1(JZERO, FV); }
            if (b1 & 0x3c) {
                J_MFC1(JT0, FPMX);
                if (set) J_ORI(JT0, JT0, b1 & 0x3c); else J_ANDI(JT0, JT0, 0x3c & ~b1);
                J_MTC1(JT0, FPMX);
            }
            /* (setFlags writes N and Z back: the lazy register takes the form (N ? 0x10000 : 0) | (Z ? 0 : 1)) */
            J_MFC1(JT0, FNZ);
            J_SRL(JT1, JT0, 15); J_SLTU(JT1, JZERO, JT1); J_SLL(JT1, JT1, 16);
            J_ANDI(JT0, JT0, 0xffff); J_SLTU(JT0, JZERO, JT0);
            J_OR(JT0, JT0, JT1);
            J_MTC1(JT0, FNZ);
            if (set && (b1 & 0x10)) {                            /* X set: the index registers lose their high bytes */
                J_MFC1(JT0, FX); J_ANDI(JT0, JT0, 0xff); J_MTC1(JT0, FX);
                J_MFC1(JT0, FY); J_ANDI(JT0, JT0, 0xff); J_MTC1(JT0, FY);
            }
        } else {
            /* The operand into S2 (a load) or the address into S2 (S3: its
             * high byte's address). */
            if (o.mode == JM_IMM) {
                J_ORI(JS2, JZERO, byte ? b1 : (b1 | b2 << 8));
            } else {
                const uint32_t adr = (o.mode == JM_DP || o.mode == JM_ILY) ? b1 : (uint32_t)(b1 | b2 << 8);
                /* An instruction that calls the C bus functions itself (a
                 * store, a read-modify-write, a pointer read): they add
                 * their accesses' speed to s_interp_bus_master, and must
                 * find the bus latch. A plain read goes through the shared
                 * routines, which see to both. */
                const bool ccall = rmw || o.mode == JM_ILY;   /* (a plain store: the store routine sees to both) */
                if (ccall) {
                    const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JB);
                    jit_latch(last_adr, last_byte);
                }
                if (o.mode == JM_ALX) {
                    jli(JT1, adr | (uint32_t)b3 << 16);
                    if (s_jit_cache) J_ADDU(JS2, JT1, JCX);
                    else { J_MFC1(JT2, FX); J_ADDU(JS2, JT1, JT2); }
                    J_SLL(JS2, JS2, 8); J_SRL(JS2, JS2, 8);
                    if (!byte) { J_ADDIU(JS3, JS2, 1); J_SLL(JS3, JS3, 8); J_SRL(JS3, JS3, 8); }
                } else if (o.mode == JM_ILY) {
                    J_MFC1(JT0, FD);
                    J_ANDI(JT1, JT0, 0xff);
                    J_SLTU(JT1, JZERO, JT1);          /* dpr not 0: 1 extra cycle */
                    J_SLL(JT2, JT1, 2); J_SLL(JT1, JT1, 1); J_ADDU(JT1, JT1, JT2);
                    J_ADDU(JS4, JS4, JT1);
                    J_ADDIU(JA0, JT0, (int)adr); J_ANDI(JA0, JA0, 0xffff);
                    J_ADDIU(JA1, JT0, (int)adr + 1); J_ANDI(JA1, JA1, 0xffff);
                    J_ADDIU(JA2, JT0, (int)adr + 2);
                    jjal_code(s_jit_c[JC_PTR24]);
                    J_ANDI(JA2, JA2, 0xffff);
                    if (s_jit_cache) J_ADDU(JS2, JV0, JCY);
                    else { J_MFC1(JT2, FY); J_ADDU(JS2, JV0, JT2); }
                    J_SLL(JS2, JS2, 8); J_SRL(JS2, JS2, 8);
                    if (!byte) { J_ADDIU(JS3, JS2, 1); J_SLL(JS3, JS3, 8); J_SRL(JS3, JS3, 8); }
                } else if (o.mode == JM_DP) {
                    J_MFC1(JT0, FD);
                    J_ANDI(JT1, JT0, 0xff);
                    J_SLTU(JT1, JZERO, JT1);          /* dpr not 0: 1 extra cycle */
                    J_SLL(JT2, JT1, 2); J_SLL(JT1, JT1, 1); J_ADDU(JT1, JT1, JT2);
                    J_ADDU(JS4, JS4, JT1);
                    J_ADDIU(JS2, JT0, (int)adr); J_ANDI(JS2, JS2, 0xffff);
                    if (!byte) { J_ADDIU(JS3, JT0, (int)adr + 1); J_ANDI(JS3, JS3, 0xffff); }
                } else {
                    J_MFC1(JT0, FDB);
                    J_SLL(JT0, JT0, 16);
                    jli(JT1, adr);
                    if (o.mode == JM_ABX || o.mode == JM_ABY) {
                        if (s_jit_cache) J_ADDU(JT3, JT1, o.mode == JM_ABX ? JCX : JCY);   /* adr + index */
                        else { J_MFC1(JT2, o.mode == JM_ABX ? FX : FY); J_ADDU(JT3, JT1, JT2); }
                        if (!jit_op_store(o.op) && !rmw) {
                            if (!xf) {
                                J_ADDIU(JS4, JS4, 6);   /* 16-bit index: 1 extra cycle */
                            } else {                    /* page crossed: 1 extra cycle */
                                J_SRL(JT4, JT3, 8); J_XORI(JT4, JT4, (int)(adr >> 8));
                                J_SLTU(JT4, JZERO, JT4);
                                J_SLL(JT5, JT4, 2); J_SLL(JT4, JT4, 1); J_ADDU(JT4, JT4, JT5);
                                J_ADDU(JS4, JS4, JT4);
                            }
                        }
                        J_MOVE(JT1, JT3);
                    }
                    J_ADDU(JS2, JT0, JT1);
                    if (o.mode != JM_ABS) { J_SLL(JS2, JS2, 8); J_SRL(JS2, JS2, 8); }   /* & 0xffffff */
                    if (!byte) { J_ADDIU(JS3, JS2, 1); J_SLL(JS3, JS3, 8); J_SRL(JS3, JS3, 8); }
                }
                if (rmw) {   /* (a store's are below) */
                    jli(JT0, ia);
                    { const int lo = jaddr(&g_interp_wlog_pc24); J_SW(JT0, lo, JB); }
                    { const int lo = jaddr(&g_interp816_cur_pc); J_SW(JT0, lo, JB); }
                }
                if (rmw) {
                    /* Read, change, write back (a word: high byte first). */
                    if (byte) { jjal_code(s_jit_c[JC_RD8]); J_MOVE(JA0, JS2); }
                    else { J_MOVE(JA1, JS3); jjal_code(s_jit_c[JC_RD16]); J_MOVE(JA0, JS2); }
                    J_ADDIU(JT0, JV0, o.op == JO_INC ? 1 : -1);
                    J_ANDI(JT0, JT0, (int)m);
                    J_SD(JT0, F_RMW, JSP);                         /* (kept across the stores) */
                    if (byte) { J_MOVE(JA1, JT0); J_MOVE(JV1, JZERO); J_MOVE(JT5, JZERO); jjal_code(s_jit_st8); J_MOVE(JA0, JS2); }
                    else {                                           /* high byte first */
                        J_SRL(JA1, JT0, 8); J_MOVE(JV1, JZERO); J_MOVE(JT5, JZERO); jjal_code(s_jit_st8); J_MOVE(JA0, JS3);
                        J_LD(JA1, F_RMW, JSP); J_MOVE(JV1, JZERO); J_MOVE(JT5, JZERO); jjal_code(s_jit_st8); J_MOVE(JA0, JS2);
                    }
                    J_LD(JT0, F_RMW, JSP);
                    jit_set_nz(JT0, byte);
                } else if (jit_op_store(o.op)) {
                    const int f = o.op == JO_STA ? FA : o.op == JO_STX ? FX : FY;
                    if (o.mode == JM_ILY) {   /* (the latch is the pointer read's by now) */
                        jli(JT0, ia);
                        { const int lo = jaddr(&g_interp_wlog_pc24); J_SW(JT0, lo, JB); }
                        { const int lo = jaddr(&g_interp816_cur_pc); J_SW(JT0, lo, JB); }
                        J_MOVE(JV1, JZERO); J_MOVE(JT5, JZERO);
                    } else {
                        jli(JV1, (last_adr & 0xFFFFFFu) << 8 | last_byte); jli(JT5, ia);
                    }
                    if (byte) {
                        if (o.op == JO_STZ) J_MOVE(JA1, JZERO); else { J_MFC1(JA1, f); }
                        jjal_code(s_jit_st8);
                        J_MOVE(JA0, JS2);
                    } else {
                        if (o.op == JO_STZ) J_MOVE(JA2, JZERO); else { J_MFC1(JA2, f); }
                        J_MOVE(JA1, JS3);
                        jjal_code(s_jit_st16);
                        J_MOVE(JA0, JS2);
                    }
                } else if (ccall) {
                    if (byte) { jjal_code(s_jit_c[JC_RD8]); J_MOVE(JA0, JS2); }
                    else { J_MOVE(JA1, JS3); jjal_code(s_jit_c[JC_RD16]); J_MOVE(JA0, JS2); }
                    J_MOVE(JS2, JV0);
                } else {
                    jit_read(byte, JS2, JS3, last_adr, last_byte);
                    J_MOVE(JS2, JV0);
                }
                if (ccall) { const int lo = jaddr(&s_interp_bus_master); J_LD(JT0, lo, JB); J_ADDU(JS4, JS4, JT0); }
            }
            /* The operation on the operand in S2. */
            switch (o.op) {
            case JO_LDA:
                if (s_jit_cache) {
                if (byte) { J_ANDI(JCA, JCA, 0xff00); J_OR(JCA, JCA, JS2); }
                else J_MOVE(JCA, JS2);
                } else {
                if (byte) { J_MFC1(JT0, FA); J_ANDI(JT0, JT0, 0xff00); J_OR(JT0, JT0, JS2); J_MTC1(JT0, FA); }
                else J_MTC1(JS2, FA);
                }
                jit_set_nz(JS2, byte);
                break;
            case JO_LDX: case JO_LDY:
                J_MTC1(JS2, o.op == JO_LDX ? FX : FY);
                jit_set_nz(JS2, byte);
                break;
            case JO_AND: case JO_ORA: case JO_EOR:
                if (s_jit_cache) {
                if (o.op == JO_AND) {
                    if (byte) { J_ORI(JT1, JS2, 0xff00); J_AND(JCA, JCA, JT1); }
                    else J_AND(JCA, JCA, JS2);
                } else if (o.op == JO_ORA) J_OR(JCA, JCA, JS2);
                else J_XOR(JCA, JCA, JS2);
                jit_set_nz(JCA, byte);
                } else {
                J_MFC1(JT0, FA);
                if (o.op == JO_AND) {
                    if (byte) J_ORI(JT1, JS2, 0xff00); else J_MOVE(JT1, JS2);
                    J_AND(JT0, JT0, JT1);
                } else if (o.op == JO_ORA) J_OR(JT0, JT0, JS2);
                else J_XOR(JT0, JT0, JS2);
                J_MTC1(JT0, FA);
                jit_set_nz(JT0, byte);
                }
                break;
            case JO_CMP: case JO_CPX: case JO_CPY:
                if (s_jit_cache) {
                J_ANDI(JT0, o.op == JO_CMP ? JCA : o.op == JO_CPX ? JCX : JCY, (int)m);
                } else {
                J_MFC1(JT0, o.op == JO_CMP ? FA : o.op == JO_CPX ? FX : FY);
                J_ANDI(JT0, JT0, (int)m);
                }
                J_XORI(JT1, JS2, (int)m);
                J_ADDU(JT0, JT0, JT1); J_ADDIU(JT0, JT0, 1);     /* (reg & m) + (value ^ m) + 1 */
                J_SRL(JT1, JT0, byte ? 8 : 16);                    /* carry: result > m */
                J_MTC1(JT1, FC);
                jit_set_nz(JT0, byte);
                break;
            case JO_BIT:
                /* Z from A & value; from memory also N and V from the
                 * value's top bits. The immediate form leaves N as it is. */
                if (s_jit_cache) {
                J_AND(JT0, JCA, JS2); J_ANDI(JT0, JT0, (int)m);
                } else {
                J_MFC1(JT0, FA);
                J_AND(JT0, JT0, JS2); J_ANDI(JT0, JT0, (int)m);
                }
                J_SLTU(JT0, JZERO, JT0);                       /* 1 = not zero */
                if (o.mode == JM_IMM) {
                    J_MFC1(JT1, FNZ);
                    J_SRL(JT1, JT1, 15); J_SLTU(JT1, JZERO, JT1);
                } else {
                    J_SRL(JT1, JS2, byte ? 7 : 15); J_ANDI(JT1, JT1, 1);
                    J_SRL(JT2, JS2, byte ? 6 : 14); J_ANDI(JT2, JT2, 1);
                    J_MTC1(JT2, FV);
                }
                J_SLL(JT1, JT1, 16); J_OR(JT0, JT0, JT1);     /* (N ? 0x10000 : 0) | (Z ? 0 : 1) */
                J_MTC1(JT0, FNZ);
                break;
            case JO_ADC: case JO_SBC:
                /* Binary mode (jit816_run keeps blocks with these out of
                 * decimal mode): (A & m) + value + C, SBC with the value's
                 * bits inverted. */
                if (o.op == JO_SBC) J_XORI(JS2, JS2, (int)m);
                J_MFC1(JT0, FA);
                J_MFC1(JT1, FC);
                J_ANDI(JT2, JT0, (int)m);
                J_ADDU(JT3, JT2, JS2); J_ADDU(JT3, JT3, JT1);           /* result */
                J_XOR(JT4, JT2, JS2); J_XOR(JT5, JS2, JT3);              /* overflow: same signs in, another out */
                jR(JT4, JZERO, JT4, 0, 0x27);                            /* nor: ~(A ^ value) */
                J_AND(JT4, JT4, JT5);
                J_SRL(JT4, JT4, byte ? 7 : 15); J_ANDI(JT4, JT4, 1);
                J_MTC1(JT4, FV);
                J_SRL(JT4, JT3, byte ? 8 : 16); J_ANDI(JT4, JT4, 1);     /* carry: result > m */
                J_MTC1(JT4, FC);
                if (byte) { J_ANDI(JT0, JT0, 0xff00); J_ANDI(JT3, JT3, 0xff); J_OR(JT3, JT3, JT0); }
                else J_ANDI(JT3, JT3, 0xffff);
                J_MTC1(JT3, FA);
                jit_set_nz(JT3, byte);
                break;
            default: break;   /* stores, INC, DEC */
            }
        }
        /* The step's end: clock, SPC pending time, count, event. */
        const uint32_t w_step = s_jit_n;
        g_jit816_words[0] += w_step - w_start;
        J_DSUBU(JR, JR, JS4);
        J_ADDIU(JS1, JS1, 1);
        jbr(6, JR, JZERO, lthunk_next); J_ORI(JT0, JZERO, i);   /* blez: the event (the delay slot names the instruction) */
        g_jit816_words[1] += s_jit_n - w_step;
        jbind(lexit[i]);
        tab[i] = (uint32_t)ipc[i] | (uint32_t)last_byte << 16 | (uint32_t)(ilen[i] - 1) << 24 | (bus ? 0u : 1u << 26);
        g_jit816_words[2] += 1;   /* (its table entry) */
    }
    /* Past the last instruction: back to the interpreter, as its event exit
     * but counted as the block's end. Then the thunks and the table. */
    jbind(label[n]);
    const uint32_t w_tail = s_jit_n;
    J_ADDIU(JT5, JZERO, -2);
    jbr(4, JZERO, JZERO, lthunk_to); J_ORI(JT0, JZERO, n - 1);
    jbind(lthunk_next);
    J_ADDIU(JT5, JZERO, -1);
    jbind(lthunk_to);
    {
        const uint32_t ta = (uint32_t)(uintptr_t)&s_jit_code[s_jit_n + 4];
        J_LUI(JT1, (int)(ta >> 16));
        J_ORI(JT1, JT1, (int)(ta & 0xffffu));
        jj(s_jit_xcommon);
        J_LUI(JT2, (int)(bank >> 16));
        for (int i = 0; i < n; i++) jput(tab[i]);
    }
    g_jit816_words[5] += s_jit_n - w_tail - (uint32_t)n;   /* (the table's words are counted with the instructions) */
    s_jit_cache = false;
    jfix_all();
    if (s_jit_full) return -1;
#ifdef N64_JIT_DUMP
    /* The code of a few blocks, for reading (objdump -b binary -m mips). */
    { extern void n64_platform_log(const char *fmt, ...);
      static uint32_t seen, dumped;
      if (++seen > (uint32_t)(N64_JIT_DUMP) && dumped < 24) {
        dumped++;
        const uint32_t w0 = at[0];
        n64_platform_log("[jitdump] block %06lx mx %lu insns %d words %lu\n", (unsigned long)(bank | ipc[0]),
                         (unsigned long)mx, n, (unsigned long)(s_jit_n - w0));
        for (int k = 0; k < n; k++)
          n64_platform_log("[jitdump] insn %04x op %02x at %lu\n", ipc[k], pg[ipc[k] & 0x1FFFu], (unsigned long)(at[k] - w0));
        for (uint32_t w = w0; w < s_jit_n; w += 8) {
          char line[100]; int o = 0;
          for (uint32_t k = w; k < w + 8 && k < s_jit_n; k++) o += snprintf(line + o, sizeof line - (size_t)o, "%08lx ", (unsigned long)s_jit_code[k]);
          n64_platform_log("[jitdump] w %s\n", line);
        }
      }
    }
#endif
    uint32_t arith = 0;
    for (int i = 0; i < n; i++) if (iop[i].op == JO_ADC || iop[i].op == JO_SBC) arith = 0x80000000u;
    for (int i = 0; i < n; i++)
        jit_add(0x80000000u | mx << 24 | bank | ipc[i], at[i], pg, (uint32_t)(n - i) | arith);
    g_jit816_stats[0]++;
    g_jit816_stats[1] += (uint32_t)n;
    return n;
}

/* Run translated code at pc24 if there is any (making it on the second
 * visit). Returns the instructions run; 0 = interpret this one. room = how
 * many instructions the step cap still allows. */
#if !SNESRECOMP_WRAM_POLL_QUIESCENT || !SNESRECOMP_FAST_READ_MAP
#error "jit816's read routines are bridge_bus_read's fast path as built with SNESRECOMP_WRAM_POLL_QUIESCENT=1 and the fast read map"
#endif
static uint32_t jit816_run(CpuState *cpu, Interp816 *in, uint32_t pc24, const uint8_t *pg, long room) {
    s_jit_cpu = cpu; s_jit_in = in;
    s_jit_store_ok = (uint8_t)cpu_wram_store_plain();
    if (!s_jit_n || g_memsel != s_jit_memsel) jit816_flush();
    const uint32_t mx = (cpu_m_flag() ? 2u : 0u) | (cpu_x_flag() ? 1u : 0u);
    const uint32_t key = 0x80000000u | mx << 24 | (pc24 & 0xFFFFFFu);
    JitEntry *e = jit_find(key);
    if (!e) {
        uint32_t *seen = &s_jit_seen[(pc24 ^ (pc24 >> 9) ^ mx) & (kJitSeen - 1)];
        if (*seen == (key ^ 0x40000000u)) return 0;   /* nothing translatable starts here */
        if (*seen != key && *seen != (key ^ 0x20000000u)) { *seen = key; return 0; }
        if (s_jit_nent > kJitEntries / 2 || s_jit_n > kJitCodeWords - 4096) jit816_flush();
        const uint32_t start = s_jit_n;
        const int n = jit_translate(pc24, pg, (mx & 2) != 0, (mx & 1) != 0);
        if (n <= 0) {
            if (n < 0) jit816_flush();
            else { s_jit_n = start; g_jit816_stats[5]++; *seen = key ^ 0x40000000u; }
            return 0;
        }
        data_cache_hit_writeback(&s_jit_code[start], (s_jit_n - start) * 4);
        inst_cache_hit_invalidate(&s_jit_code[start], (s_jit_n - start) * 4);
        *seen = key ^ 0x20000000u;
        e = jit_find(key);
        if (!e) return 0;
    }
    if (e->pg != pg || (long)kJitRunMax + 2 * kJitMaxInsns >= room) return 0;
    if ((e->n & 0x80000000u) && cpu_flag_D()) return 0;   /* decimal arithmetic: the interpreter's */
    const uint32_t ran = ((JitEnterFn)(void *)&s_jit_code[0])(&s_jit_code[e->code], in);
    g_jit816_stats[2]++;
    g_jit816_stats[3] += ran;
    return ran;
}
