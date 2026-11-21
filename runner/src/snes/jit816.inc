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
static uint8_t s_jit_speed[kCpuFastMapPages];   /* master clocks per access, by 8 KB page (RAM and ROM pages) */
typedef struct { uint32_t key; uint32_t code; const uint8_t *pg; uint32_t n; } JitEntry;   /* n: bit 31 = holds ADC/SBC (binary mode only) */
static JitEntry s_jit_ent[kJitEntries];   /* key: 0x80000000 | mx << 24 | pc24 */
static uint32_t s_jit_nent;
static uint32_t s_jit_seen[kJitSeen];     /* block starts reached once */
static uint8_t s_jit_memsel;
static bool s_jit_full;
static uint32_t s_jit_fp;                 /* a looping block's A, X, Y and write count at its last lap */
uint32_t g_jit816_prev;                   /* pc24 of the last instruction a block ran */
uint32_t g_jit816_ended[256];             /* by opcode: translations that stopped at it (no code for it yet) */
uint32_t g_jit816_stats[6];               /* blocks, instructions translated, runs, instructions run, flushes, refused */
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
       JT3 = 11, JT4 = 12, JT5 = 13, JT6 = 14, JT7 = 15, JT8 = 24, JT9 = 25, JS0 = 16, JS1 = 17, JS2 = 18, JS3 = 19, JS4 = 20, JS5 = 21, JS6 = 22, JSP = 29, JRA = 31 };
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
#define J_AND(rd, rs, rt) jR(rs, rt, rd, 0, 0x24)
#define J_OR(rd, rs, rt) jR(rs, rt, rd, 0, 0x25)
#define J_XOR(rd, rs, rt) jR(rs, rt, rd, 0, 0x26)
#define J_SLTU(rd, rs, rt) jR(rs, rt, rd, 0, 0x2b)
#define J_SLL(rd, rt, sa) jR(0, rt, rd, sa, 0)
#define J_SRL(rd, rt, sa) jR(0, rt, rd, sa, 2)
#define J_MOVE(rd, rs) J_OR(rd, rs, JZERO)
#define J_NOP() jput(0)
#define J_JR(rs) jR(rs, 0, 0, 0, 8)
#define J_MFC1(rt, fs) jput(0x44000000u | (uint32_t)(rt) << 16 | (uint32_t)(fs) << 11)
#define J_MTC1(rt, fs) jput(0x44800000u | (uint32_t)(rt) << 16 | (uint32_t)(fs) << 11)
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
static int jaddr(const void *p) {
    const uint32_t a = (uint32_t)(uintptr_t)p;
    const int lo = (int16_t)(a & 0xffffu);
    J_LUI(JAT, (int)(((a - (uint32_t)lo) >> 16) & 0xffffu));
    return lo;
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

static void jit816_flush(void) {
    memset(s_jit_ent, 0, sizeof s_jit_ent);
    memset(s_jit_seen, 0, sizeof s_jit_seen);
    s_jit_nent = 0;
    s_jit_n = 0;
    s_jit_full = false;
    s_jit_memsel = g_memsel;
    g_jit816_stats[4]++;
    /* The way in and out, shared by every block. */
    enum { FRAME = 112, F_RA2 = 40 };   /* 0-39: the called C functions' argument area; 40: a routine's return address */
    J_ADDIU(JSP, JSP, -FRAME);
    J_SD(JRA, 104, JSP); J_SD(JS0, 96, JSP); J_SD(JS1, 88, JSP); J_SD(JS2, 80, JSP);
    J_SD(JS3, 72, JSP); J_SD(JS4, 64, JSP); J_SD(JS5, 56, JSP); J_SD(JS6, 48, JSP);
    J_MOVE(JS0, JA1);
    J_ORI(JS5, JZERO, kJitLaps);
    J_JR(JA0);
    J_MOVE(JS1, JZERO);
    s_jit_epilogue = s_jit_n;
    J_MOVE(JV0, JS1);
    J_LD(JRA, 104, JSP); J_LD(JS0, 96, JSP); J_LD(JS1, 88, JSP); J_LD(JS2, 80, JSP);
    J_LD(JS3, 72, JSP); J_LD(JS4, 64, JSP); J_LD(JS5, 56, JSP); J_LD(JS6, 48, JSP);
    J_JR(JRA);
    J_ADDIU(JSP, JSP, FRAME);

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
        { const int lo = jaddr(g_cpu_fast_read_page); J_ADDU(JAT, JAT, JT9); J_LW(JT9, lo, JAT); }
        br0 = s_jit_n; jI(4, JT9, JZERO, 0);                 /* beqz: not a plain page */
        if (!word) {
            J_ANDI(JT6, JA0, 0x1fff);
            J_ADDU(JT9, JT9, JT6);
            J_LBU(JV0, 0, JT9);
            { const int lo = jaddr(s_jit_speed); J_ADDU(JAT, JAT, JT8); J_LBU(JT8, lo, JAT); }
            J_ADDU(JS4, JS4, JT8);
            { const int lo = jaddr(&g_open_bus); J_SB(JV0, lo, JAT); }
            { const int lo = jaddr(&g_snes); J_LW(JT9, lo, JAT); }
            J_LW(JT9, (int)offsetof(Snes, cart), JT9);
            J_JR(JRA);
            J_SW(JA0, (int)offsetof(Cart, cpuBusAddress), JT9);
        } else {
            J_SRL(JT6, JA1, kCpuFastMapPageShift);
            J_SLL(JT7, JT6, 2);
            { const int lo = jaddr(g_cpu_fast_read_page); J_ADDU(JAT, JAT, JT7); J_LW(JT7, lo, JAT); }
            br1 = s_jit_n; jI(4, JT7, JZERO, 0);
            J_ANDI(JV0, JA0, 0x1fff);
            J_ADDU(JT9, JT9, JV0);
            J_LBU(JV0, 0, JT9);                              /* low byte */
            J_ANDI(JT9, JA1, 0x1fff);
            J_ADDU(JT7, JT7, JT9);
            J_LBU(JT7, 0, JT7);                              /* high byte */
            { const int lo = jaddr(s_jit_speed); J_ADDU(JT9, JAT, JT8); J_LBU(JT8, lo, JT9);
              J_ADDU(JT9, JAT, JT6); J_LBU(JT6, lo, JT9); }
            J_ADDU(JS4, JS4, JT8);
            J_ADDU(JS4, JS4, JT6);
            { const int lo = jaddr(&g_open_bus); J_SB(JT7, lo, JAT); }
            J_SLL(JT7, JT7, 8);
            J_OR(JV0, JV0, JT7);
            { const int lo = jaddr(&g_snes); J_LW(JT9, lo, JAT); }
            J_LW(JT9, (int)offsetof(Snes, cart), JT9);
            J_JR(JRA);
            J_SW(JA1, (int)offsetof(Cart, cpuBusAddress), JT9);
        }
        /* slow: the latch, then the C function; its accesses' speed is in
         * s_interp_bus_master. */
        s_jit_code[br0] |= (s_jit_n - br0 - 1) & 0xffffu;
        if (word) s_jit_code[br1] |= (s_jit_n - br1 - 1) & 0xffffu;
        J_SD(JRA, F_RA2, JSP);
        { const int lo = jaddr(&g_open_bus); J_SB(JV1, lo, JAT); }
        J_SRL(JT6, JV1, 8);
        { const int lo = jaddr(&g_snes); J_LW(JT9, lo, JAT); }
        J_LW(JT9, (int)offsetof(Snes, cart), JT9);
        J_SW(JT6, (int)offsetof(Cart, cpuBusAddress), JT9);
        { const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JAT); }
        jjal(word ? (void *)jit_rd16 : (void *)jit_rd8);
        J_NOP();
        { const int lo = jaddr(&s_interp_bus_master); J_LD(JT8, lo, JAT); }
        J_ADDU(JS4, JS4, JT8);
        J_LD(JRA, F_RA2, JSP);
        J_JR(JRA);
        J_NOP();
    }
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
    JitOp r = { JO_NONE, JM_NONE };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].opc == opc) { r.op = k[i].op; r.mode = k[i].mode; break; }
    return r;
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
    J_SB(JT5, lo, JAT);
    lo = jaddr(&g_snes);
    J_LW(JT5, lo, JAT);
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
#ifdef N64_JIT_EXITS
    { int lo = jaddr(&g_jit816_exit[s_jit_exit_kind]); J_LW(JT0, lo, JAT); J_ADDIU(JT0, JT0, 1); J_SW(JT0, lo, JAT);
      if (s_jit_exit_kind == 2) { lo = jaddr(&g_jit816_exit_op[s_jit_exit_op]); J_LW(JT0, lo, JAT); J_ADDIU(JT0, JT0, 1); J_SW(JT0, lo, JAT); } }
#endif
    jli(JT0, pc);
    J_SH(JT0, (int)offsetof(Interp816, pc), JS0);
    jli(JT0, last_pc24);
    const int lo = jaddr(&g_jit816_prev);
    jj(s_jit_epilogue);
    J_SW(JT0, lo, JAT);
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
    int label[kJitMaxInsns + 1], lexit[kJitMaxInsns];
    for (int i = 0; i <= n; i++) label[i] = jlabel();
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
                if (sel == 0) { J_MFC1(JT0, FNZ); J_NOP(); J_SRL(JT0, JT0, 15); J_SLTU(JT0, JZERO, JT0); }
                else if (sel == 1) { J_MFC1(JT0, FV); J_NOP(); }
                else if (sel == 2) { J_MFC1(JT0, FC); J_NOP(); }
                else { J_MFC1(JT0, FNZ); J_NOP(); J_ANDI(JT0, JT0, 0xffff); J_SLTIU(JT0, JT0, 1); }
                jbr(want ? 5 : 4, JT0, JZERO, ltaken);
                J_ORI(JS4, JZERO, (int)fixed);      /* (delay slot) not taken: 2 cycles */
                /* not taken */
                J_DMFC1(JT1, FMC); J_NOP(); J_DADDU(JT1, JT1, JS4); J_DMTC1(JT1, FMC); J_DMTC1(JT1, FCMC);
                { const int lo = jaddr(&s_apu_pending_master); J_LD(JT2, lo, JAT); J_DADDU(JT2, JT2, JS4); J_SD(JT2, lo, JAT); }
                J_ADDIU(JS1, JS1, 1);
                { const int lo = jaddr(&s_lean_event); J_LD(JT3, lo, JAT); J_SLTU(JT4, JT1, JT3);
                  jbr(4, JT4, JZERO, lexit[i]); J_NOP(); }
                { const int lskip = label[i + 1]; jbr(4, JZERO, JZERO, lskip); J_NOP(); }
            }
            /* taken: one more internal cycle (BRA's is in its count) */
            jbind(ltaken);
            J_ORI(JS4, JZERO, (int)(fixed + (o.op == JO_BR ? 6u : 0u)));
            J_DMFC1(JT1, FMC); J_NOP(); J_DADDU(JT1, JT1, JS4); J_DMTC1(JT1, FMC); J_DMTC1(JT1, FCMC);
            { const int lo = jaddr(&s_apu_pending_master); J_LD(JT2, lo, JAT); J_DADDU(JT2, JT2, JS4); J_SD(JT2, lo, JAT); }
            J_ADDIU(JS1, JS1, 1);
            if (tl >= 0) {
                const int lo = jaddr(&s_lean_event); J_LD(JT3, lo, JAT); J_SLTU(JT4, JT1, JT3);
                if (tl <= i) {
                    /* A loop inside the block. While it makes progress --
                     * A, X or Y differ from the lap before, or something
                     * was written -- it stays. A lap that changed none of
                     * them may be a wait for an interrupt: after a few of
                     * those in a row it leaves by the branch, so that the
                     * big loop's wait detector sees the loop's head. (And
                     * it leaves after kJitRunMax instructions in any case:
                     * the caller's step cap.) */
                    J_MFC1(JT0, FA); J_MFC1(JT1, FX); J_MFC1(JT2, FY); J_NOP();
                    J_SLL(JT1, JT1, 8); J_XOR(JT0, JT0, JT1);
                    J_SLL(JT2, JT2, 16); J_XOR(JT0, JT0, JT2);
                    { const int lo = jaddr((const uint8_t *)&g_interp_bridge_write_epoch + sizeof g_interp_bridge_write_epoch - 4);
                      J_LW(JT1, lo, JAT); }
                    J_SLL(JT2, JT1, 24); J_XOR(JT0, JT0, JT2); J_XOR(JT0, JT0, JT1);
                    { const int lo = jaddr(&s_jit_fp); J_LW(JT1, lo, JAT); J_SW(JT0, lo, JAT); }
                    J_XOR(JT1, JT1, JT0);
                    jI(4, JT1, JZERO, 2);                 /* the same as last lap: one fewer left */
                    J_ADDIU(JS5, JS5, -1);
                    J_ORI(JS5, JZERO, kJitLaps);          /* progress: the full allowance again */
                    J_SLTU(JT5, JZERO, JS5);
                    J_AND(JT4, JT4, JT5);
                    J_SLTIU(JT5, JS1, kJitRunMax);
                    J_AND(JT4, JT4, JT5);
                }
                jbr(5, JT4, JZERO, label[tl]); J_NOP();
            }
            jit_latch(last_adr, last_byte);
            JIT_EXIT_KIND(1, 0);
            jit_exit(target, ia);
            /* the not-taken path's event exit */
            jbind(lexit[i]);
            jit_latch(last_adr, last_byte);
            JIT_EXIT_KIND(0, 0);
            jit_exit(ipc[i + 1], ia);
            continue;
        }

        if (jit_op_xfer(o.op)) {
            const bool call = o.op == JO_JSR || o.op == JO_JSL, ret = o.op == JO_RTS || o.op == JO_RTL;
            const bool lng = o.op == JO_JSL || o.op == JO_RTL || o.op == JO_JML;
            const uint32_t target = ret ? 0 : jit_xfer_target(ia, pg, o);
            if (ret) {
                /* Leaving S above the run's entry depth is the run's end:
                 * the big loop's to do. (i == 0: jit_scan.) */
                J_MFC1(JS2, FS); J_NOP();
                J_ADDIU(JT0, JS2, lng ? 3 : 2); J_ANDI(JT0, JT0, 0xffff);
                { const int lo = jaddr(&s_jit_ret_limit); J_LW(JT1, lo, JAT); }
                J_SLTU(JT2, JT1, JT0);
                jbr(5, JT2, JZERO, lexit[i]); J_NOP();
            }
            J_ORI(JS4, JZERO, (int)fixed);
            if (call) {
                /* pushByte(K) for a long call, then pushWord(the last operand byte's address) */
                { const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JAT); }
                jit_latch(last_adr, last_byte);
                jli(JT0, ia);
                { const int lo = jaddr(&g_interp_wlog_pc24); J_SW(JT0, lo, JAT); }
                { const int lo = jaddr(&g_interp816_cur_pc); J_SW(JT0, lo, JAT); }
                J_MFC1(JS2, FS); J_NOP();
                if (lng) {
                    J_MFC1(JA1, FPB); J_NOP(); jjal(jit_wr8); J_MOVE(JA0, JS2);
                    J_ADDIU(JS2, JS2, -1); J_ANDI(JS2, JS2, 0xffff);
                }
                const uint32_t r16 = (ipc[i] + ilen[i] - 1u) & 0xFFFFu;
                J_ORI(JA1, JZERO, (int)(r16 >> 8)); jjal(jit_wr8); J_MOVE(JA0, JS2);
                J_ADDIU(JS2, JS2, -1); J_ANDI(JS2, JS2, 0xffff);
                J_ORI(JA1, JZERO, (int)(r16 & 0xff)); jjal(jit_wr8); J_MOVE(JA0, JS2);
                J_ADDIU(JS2, JS2, -1); J_ANDI(JS2, JS2, 0xffff);
                J_MTC1(JS2, FS);
                { const int lo = jaddr(&s_interp_bus_master); J_LD(JT0, lo, JAT); J_ADDU(JS4, JS4, JT0); }
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
                { const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JAT); }
                jjal(jit_rd8); J_MOVE(JA0, JS2);
                J_SLL(JV0, JV0, 8); J_OR(JS3, JS3, JV0);
                J_ADDIU(JS3, JS3, 1); J_ANDI(JS3, JS3, 0xffff);
                if (lng) {
                    J_ADDIU(JS2, JS2, 1); J_ANDI(JS2, JS2, 0xffff);
                    jjal(jit_rd8); J_MOVE(JA0, JS2);
                    J_ANDI(JS6, JV0, 0xff);
                    J_MTC1(JS6, FPB);
                }
                { const int lo = jaddr(&s_interp_bus_master); J_LD(JT0, lo, JAT); J_ADDU(JS4, JS4, JT0); }
                J_MTC1(JS2, FS);
            } else jit_latch(last_adr, last_byte);
            if (!ret && lng) { J_ORI(JT0, JZERO, (int)(target >> 16)); J_MTC1(JT0, FPB); }
            J_DMFC1(JT1, FMC); J_NOP(); J_DADDU(JT1, JT1, JS4); J_DMTC1(JT1, FMC); J_DMTC1(JT1, FCMC);
            { const int lo = jaddr(&s_apu_pending_master); J_LD(JT2, lo, JAT); J_DADDU(JT2, JT2, JS4); J_SD(JT2, lo, JAT); }
            J_ADDIU(JS1, JS1, 1);
            JIT_EXIT_KIND(1, 0);
            /* The lean loop's "last pc": for a call or a return, just below
             * the target (they are not loop heads: the loop goes on there);
             * for a jump, the jump (back is a loop head, as a branch). */
            if (ret) {
#ifdef N64_JIT_EXITS
                { const int lo = jaddr(&g_jit816_exit[1]); J_LW(JT0, lo, JAT); J_ADDIU(JT0, JT0, 1); J_SW(JT0, lo, JAT); }
#endif
                J_SH(JS3, (int)offsetof(Interp816, pc), JS0);
                J_ADDIU(JT0, JS3, -1); J_ANDI(JT0, JT0, 0xffff);
                if (lng) J_SLL(JT1, JS6, 16); else jli(JT1, bank);
                J_OR(JT0, JT0, JT1);
                const int lo = jaddr(&g_jit816_prev);
                jj(s_jit_epilogue);
                J_SW(JT0, lo, JAT);
            } else {
                jit_exit((uint16_t)target, call ? ((target & 0xFF0000u) | ((target - 1u) & 0xFFFFu)) : ia);
            }
            /* a return the big loop must make: nothing done, nothing run */
            jbind(lexit[i]);
            if (ret) { JIT_EXIT_KIND(2, opc); jit_exit(ipc[i], ia); }
            continue;
        }

        J_ORI(JS4, JZERO, (int)fixed);
        if (o.mode == JM_NONE) {
            switch (o.op) {
            case JO_INX: case JO_DEX: case JO_INY: case JO_DEY: {
                const int f = (o.op == JO_INX || o.op == JO_DEX) ? FX : FY;
                J_MFC1(JT0, f); J_NOP();
                J_ADDIU(JT0, JT0, (o.op == JO_INX || o.op == JO_INY) ? 1 : -1);
                J_ANDI(JT0, JT0, xf ? 0xff : 0xffff);
                J_MTC1(JT0, f);
                jit_set_nz(JT0, xf);
                break; }
            case JO_TAX: case JO_TAY:
                J_MFC1(JT0, FA); J_NOP();
                if (xf) J_ANDI(JT0, JT0, 0xff);
                J_MTC1(JT0, o.op == JO_TAX ? FX : FY);
                jit_set_nz(JT0, xf);
                break;
            case JO_TXA: case JO_TYA:
                J_MFC1(JT0, o.op == JO_TXA ? FX : FY); J_NOP();
                if (mf) {
                    J_MFC1(JT1, FA); J_NOP();
                    J_ANDI(JT1, JT1, 0xff00); J_ANDI(JT0, JT0, 0xff); J_OR(JT0, JT0, JT1);
                }
                J_MTC1(JT0, FA);
                jit_set_nz(JT0, mf);
                break;
            case JO_CLC: J_MTC1(JZERO, FC); break;
            case JO_SEC: J_ORI(JT0, JZERO, 1); J_MTC1(JT0, FC); break;
            case JO_ASLA: case JO_LSRA: case JO_ROLA: case JO_RORA: case JO_INCA: case JO_DECA: {
                /* T0 = A, T1 = the new value (its low 8 or 16 bits count) */
                J_MFC1(JT0, FA); J_NOP();
                if (o.op == JO_ASLA) {
                    J_SRL(JT2, JT0, mf ? 7 : 15); J_ANDI(JT2, JT2, 1); J_MTC1(JT2, FC);
                    J_SLL(JT1, JT0, 1);
                } else if (o.op == JO_LSRA) {
                    J_ANDI(JT2, JT0, 1); J_MTC1(JT2, FC);
                    if (mf) { J_SRL(JT1, JT0, 1); J_ANDI(JT1, JT1, 0x7f); } else J_SRL(JT1, JT0, 1);
                } else if (o.op == JO_ROLA) {
                    J_MFC1(JT2, FC); J_NOP();
                    J_SLL(JT1, JT0, 1); J_OR(JT1, JT1, JT2);
                    J_SRL(JT2, JT1, mf ? 8 : 16); J_ANDI(JT2, JT2, 1); J_MTC1(JT2, FC);
                } else if (o.op == JO_RORA) {
                    J_MFC1(JT2, FC); J_NOP();
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
                J_MFC1(JT0, FA); J_NOP();
                J_SRL(JT1, JT0, 8); J_ANDI(JT1, JT1, 0xff);      /* high */
                J_ANDI(JT0, JT0, 0xff); J_SLL(JT0, JT0, 8); J_OR(JT0, JT0, JT1);
                J_MTC1(JT0, FA);
                jit_set_nz(JT1, true);
                break;
            case JO_PHA: case JO_PHX: case JO_PHY: {
                const int f = o.op == JO_PHA ? FA : o.op == JO_PHX ? FX : FY;
                { const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JAT); }
                jit_latch(last_adr, last_byte);
                jli(JT0, ia);
                { const int lo = jaddr(&g_interp_wlog_pc24); J_SW(JT0, lo, JAT); }
                { const int lo = jaddr(&g_interp816_cur_pc); J_SW(JT0, lo, JAT); }
                J_MFC1(JS2, FS); J_NOP();
                J_MFC1(JS3, f); J_NOP();
                if (byte) {
                    J_MOVE(JA1, JS3); jjal(jit_wr8); J_MOVE(JA0, JS2);
                    J_ADDIU(JS2, JS2, -1);
                } else {                                         /* high byte, then low */
                    J_SRL(JA1, JS3, 8); jjal(jit_wr8); J_MOVE(JA0, JS2);
                    J_ADDIU(JS2, JS2, -1); J_ANDI(JS2, JS2, 0xffff);
                    J_MOVE(JA1, JS3); jjal(jit_wr8); J_MOVE(JA0, JS2);
                    J_ADDIU(JS2, JS2, -1);
                }
                J_ANDI(JS2, JS2, 0xffff);
                J_MTC1(JS2, FS);
                { const int lo = jaddr(&s_interp_bus_master); J_LD(JT0, lo, JAT); J_ADDU(JS4, JS4, JT0); }
                break; }
            case JO_PLA: case JO_PLX: case JO_PLY: {
                const int f = o.op == JO_PLA ? FA : o.op == JO_PLX ? FX : FY;
                J_MFC1(JS2, FS); J_NOP();
                J_ADDIU(JS2, JS2, 1); J_ANDI(JS2, JS2, 0xffff);
                jit_read(true, JS2, 0, last_adr, last_byte);
                J_MOVE(JS3, JV0);
                if (!byte) {                                     /* low byte, then high (two byte reads, as pullWord) */
                    J_ADDIU(JS2, JS2, 1); J_ANDI(JS2, JS2, 0xffff);
                    /* (through the C function: the bus latch is the first read's now, not the operand's) */
                    { const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JAT); }
                    jjal(jit_rd8); J_MOVE(JA0, JS2);
                    { const int lo = jaddr(&s_interp_bus_master); J_LD(JT0, lo, JAT); J_ADDU(JS4, JS4, JT0); }
                    J_SLL(JV0, JV0, 8); J_OR(JS3, JS3, JV0);
                }
                J_MTC1(JS2, FS);
                if (byte && o.op == JO_PLA) {                    /* 8-bit A keeps its high byte */
                    J_MFC1(JT0, FA); J_NOP(); J_ANDI(JT0, JT0, 0xff00); J_OR(JT0, JT0, JS3); J_MTC1(JT0, FA);
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
                J_MFC1(JT0, FPMX); J_NOP();
                if (set) J_ORI(JT0, JT0, b1 & 0x3c); else J_ANDI(JT0, JT0, 0x3c & ~b1);
                J_MTC1(JT0, FPMX);
            }
            /* (setFlags writes N and Z back: the lazy register takes the form (N ? 0x10000 : 0) | (Z ? 0 : 1)) */
            J_MFC1(JT0, FNZ); J_NOP();
            J_SRL(JT1, JT0, 15); J_SLTU(JT1, JZERO, JT1); J_SLL(JT1, JT1, 16);
            J_ANDI(JT0, JT0, 0xffff); J_SLTU(JT0, JZERO, JT0);
            J_OR(JT0, JT0, JT1);
            J_MTC1(JT0, FNZ);
            if (set && (b1 & 0x10)) {                            /* X set: the index registers lose their high bytes */
                J_MFC1(JT0, FX); J_NOP(); J_ANDI(JT0, JT0, 0xff); J_MTC1(JT0, FX);
                J_MFC1(JT0, FY); J_NOP(); J_ANDI(JT0, JT0, 0xff); J_MTC1(JT0, FY);
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
                const bool ccall = jit_op_store(o.op) || rmw || o.mode == JM_ILY;
                if (ccall) {
                    const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JAT);
                    jit_latch(last_adr, last_byte);
                }
                if (o.mode == JM_ALX) {
                    J_MFC1(JT2, FX); J_NOP();
                    jli(JT1, adr | (uint32_t)b3 << 16);
                    J_ADDU(JS2, JT1, JT2);
                    J_SLL(JS2, JS2, 8); J_SRL(JS2, JS2, 8);
                    if (!byte) { J_ADDIU(JS3, JS2, 1); J_SLL(JS3, JS3, 8); J_SRL(JS3, JS3, 8); }
                } else if (o.mode == JM_ILY) {
                    J_MFC1(JT0, FD); J_NOP();
                    J_ANDI(JT1, JT0, 0xff);
                    J_SLTU(JT1, JZERO, JT1);          /* dpr not 0: 1 extra cycle */
                    J_SLL(JT2, JT1, 2); J_SLL(JT1, JT1, 1); J_ADDU(JT1, JT1, JT2);
                    J_ADDU(JS4, JS4, JT1);
                    J_ADDIU(JA0, JT0, (int)adr); J_ANDI(JA0, JA0, 0xffff);
                    J_ADDIU(JA1, JT0, (int)adr + 1); J_ANDI(JA1, JA1, 0xffff);
                    J_ADDIU(JA2, JT0, (int)adr + 2);
                    jjal(jit_ptr24);
                    J_ANDI(JA2, JA2, 0xffff);
                    J_MFC1(JT2, FY); J_NOP();
                    J_ADDU(JS2, JV0, JT2);
                    J_SLL(JS2, JS2, 8); J_SRL(JS2, JS2, 8);
                    if (!byte) { J_ADDIU(JS3, JS2, 1); J_SLL(JS3, JS3, 8); J_SRL(JS3, JS3, 8); }
                } else if (o.mode == JM_DP) {
                    J_MFC1(JT0, FD); J_NOP();
                    J_ANDI(JT1, JT0, 0xff);
                    J_SLTU(JT1, JZERO, JT1);          /* dpr not 0: 1 extra cycle */
                    J_SLL(JT2, JT1, 2); J_SLL(JT1, JT1, 1); J_ADDU(JT1, JT1, JT2);
                    J_ADDU(JS4, JS4, JT1);
                    J_ADDIU(JS2, JT0, (int)adr); J_ANDI(JS2, JS2, 0xffff);
                    if (!byte) { J_ADDIU(JS3, JT0, (int)adr + 1); J_ANDI(JS3, JS3, 0xffff); }
                } else {
                    J_MFC1(JT0, FDB); J_NOP();
                    J_SLL(JT0, JT0, 16);
                    jli(JT1, adr);
                    if (o.mode == JM_ABX || o.mode == JM_ABY) {
                        J_MFC1(JT2, o.mode == JM_ABX ? FX : FY); J_NOP();
                        J_ADDU(JT3, JT1, JT2);          /* adr + index */
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
                if (jit_op_store(o.op) || rmw) {
                    jli(JT0, ia);
                    { const int lo = jaddr(&g_interp_wlog_pc24); J_SW(JT0, lo, JAT); }
                    { const int lo = jaddr(&g_interp816_cur_pc); J_SW(JT0, lo, JAT); }
                }
                if (rmw) {
                    /* Read, change, write back (a word: high byte first). */
                    if (byte) { jjal(jit_rd8); J_MOVE(JA0, JS2); }
                    else { J_MOVE(JA1, JS3); jjal(jit_rd16); J_MOVE(JA0, JS2); }
                    J_ADDIU(JS6, JV0, o.op == JO_INC ? 1 : -1);
                    J_ANDI(JS6, JS6, (int)m);
                    if (byte) { J_MOVE(JA1, JS6); jjal(jit_wr8); J_MOVE(JA0, JS2); }
                    else { J_MOVE(JA2, JS6); J_MOVE(JA1, JS3); jjal(jit_wr16r); J_MOVE(JA0, JS2); }
                    jit_set_nz(JS6, byte);
                } else if (jit_op_store(o.op)) {
                    jli(JT0, ia);
                    { const int lo = jaddr(&g_interp_wlog_pc24); J_SW(JT0, lo, JAT); }
                    { const int lo = jaddr(&g_interp816_cur_pc); J_SW(JT0, lo, JAT); }
                    const int f = o.op == JO_STA ? FA : o.op == JO_STX ? FX : FY;
                    if (byte) {
                        if (o.op == JO_STZ) J_MOVE(JA1, JZERO); else { J_MFC1(JA1, f); J_NOP(); }
                        jjal(jit_wr8);
                        J_MOVE(JA0, JS2);
                    } else {
                        if (o.op == JO_STZ) J_MOVE(JA2, JZERO); else { J_MFC1(JA2, f); J_NOP(); }
                        J_MOVE(JA1, JS3);
                        jjal(jit_wr16);
                        J_MOVE(JA0, JS2);
                    }
                } else if (ccall) {
                    if (byte) { jjal(jit_rd8); J_MOVE(JA0, JS2); }
                    else { J_MOVE(JA1, JS3); jjal(jit_rd16); J_MOVE(JA0, JS2); }
                    J_MOVE(JS2, JV0);
                } else {
                    jit_read(byte, JS2, JS3, last_adr, last_byte);
                    J_MOVE(JS2, JV0);
                }
                if (ccall) { const int lo = jaddr(&s_interp_bus_master); J_LD(JT0, lo, JAT); J_ADDU(JS4, JS4, JT0); }
            }
            /* The operation on the operand in S2. */
            switch (o.op) {
            case JO_LDA:
                if (byte) { J_MFC1(JT0, FA); J_NOP(); J_ANDI(JT0, JT0, 0xff00); J_OR(JT0, JT0, JS2); J_MTC1(JT0, FA); }
                else J_MTC1(JS2, FA);
                jit_set_nz(JS2, byte);
                break;
            case JO_LDX: case JO_LDY:
                J_MTC1(JS2, o.op == JO_LDX ? FX : FY);
                jit_set_nz(JS2, byte);
                break;
            case JO_AND: case JO_ORA: case JO_EOR:
                J_MFC1(JT0, FA); J_NOP();
                if (o.op == JO_AND) {
                    if (byte) J_ORI(JT1, JS2, 0xff00); else J_MOVE(JT1, JS2);
                    J_AND(JT0, JT0, JT1);
                } else if (o.op == JO_ORA) J_OR(JT0, JT0, JS2);
                else J_XOR(JT0, JT0, JS2);
                J_MTC1(JT0, FA);
                jit_set_nz(JT0, byte);
                break;
            case JO_CMP: case JO_CPX: case JO_CPY:
                J_MFC1(JT0, o.op == JO_CMP ? FA : o.op == JO_CPX ? FX : FY); J_NOP();
                J_ANDI(JT0, JT0, (int)m);
                J_XORI(JT1, JS2, (int)m);
                J_ADDU(JT0, JT0, JT1); J_ADDIU(JT0, JT0, 1);     /* (reg & m) + (value ^ m) + 1 */
                J_SRL(JT1, JT0, byte ? 8 : 16);                    /* carry: result > m */
                J_MTC1(JT1, FC);
                jit_set_nz(JT0, byte);
                break;
            case JO_BIT:
                /* Z from A & value; from memory also N and V from the
                 * value's top bits. The immediate form leaves N as it is. */
                J_MFC1(JT0, FA); J_NOP();
                J_AND(JT0, JT0, JS2); J_ANDI(JT0, JT0, (int)m);
                J_SLTU(JT0, JZERO, JT0);                       /* 1 = not zero */
                if (o.mode == JM_IMM) {
                    J_MFC1(JT1, FNZ); J_NOP();
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
                J_MFC1(JT0, FA); J_NOP();
                J_MFC1(JT1, FC); J_NOP();
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
        J_DMFC1(JT1, FMC); J_NOP(); J_DADDU(JT1, JT1, JS4); J_DMTC1(JT1, FMC); J_DMTC1(JT1, FCMC);
        { const int lo = jaddr(&s_apu_pending_master); J_LD(JT2, lo, JAT); J_DADDU(JT2, JT2, JS4); J_SD(JT2, lo, JAT); }
        J_ADDIU(JS1, JS1, 1);
        { const int lo = jaddr(&s_lean_event); J_LD(JT3, lo, JAT); J_SLTU(JT4, JT1, JT3);
          jbr(5, JT4, JZERO, label[i + 1]); J_NOP(); }
        jbind(lexit[i]);
        if (!bus) jit_latch(last_adr, last_byte);
        JIT_EXIT_KIND(0, 0);
        jit_exit(ipc[i + 1], ia);
    }
    /* Past the last instruction: back to the interpreter. */
    jbind(label[n]);
    {
        const uint32_t la = bank | ((ipc[n - 1] + ilen[n - 1] - 1u) & 0xFFFFu);
        const JitOp lo_ = iop[n - 1];
        const bool ldata = lo_.mode == JM_DP || lo_.mode == JM_ABS || lo_.mode == JM_ABX || lo_.mode == JM_ABY ||
                           lo_.mode == JM_ALX || lo_.mode == JM_ILY ||
                           (lo_.op >= JO_PHA && lo_.op <= JO_PLY);
        if (!ldata) jit_latch(la, pg[la & 0x1FFFu]);
        JIT_EXIT_KIND(2, pg[ipc[n] & 0x1FFFu]);
        jit_exit(ipc[n], bank | ipc[n - 1]);
    }
    jfix_all();
    if (s_jit_full) return -1;
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
    if (!s_jit_n || g_memsel != s_jit_memsel) jit816_flush();
    const uint32_t mx = (cpu_m_flag() ? 2u : 0u) | (cpu_x_flag() ? 1u : 0u);
    const uint32_t key = 0x80000000u | mx << 24 | (pc24 & 0xFFFFFFu);
    JitEntry *e = jit_find(key);
    if (!e) {
        uint32_t *seen = &s_jit_seen[(pc24 ^ (pc24 >> 9) ^ mx) & (kJitSeen - 1)];
        if (*seen == (key ^ 0x40000000u)) return 0;   /* nothing translatable starts here */
        if (*seen != key) { *seen = key; return 0; }
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
