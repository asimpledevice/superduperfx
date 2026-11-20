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
 * The lean loop interprets whatever a block leaves. A block is made the
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
 * finished, s2/s3 = values that live across a bus call, s4 = this
 * instruction's master clocks so far. */

void data_cache_hit_writeback(volatile const void *, unsigned long);
void inst_cache_hit_invalidate(volatile void *, unsigned long);
extern const uint8_t cyclesPerOpcode[256];   /* interp816.c */

enum { kJitCodeWords = 49152, kJitEntries = 2048, kJitMaxInsns = 32, kJitSeen = 512 };
static uint32_t s_jit_code[kJitCodeWords] __attribute__((aligned(32)));
static uint32_t s_jit_n;           /* words used */
static uint32_t s_jit_epilogue;    /* word index */
typedef struct { uint32_t key; uint32_t code; const uint8_t *pg; uint32_t n; } JitEntry;
static JitEntry s_jit_ent[kJitEntries];   /* key: 0x80000000 | mx << 24 | pc24 */
static uint32_t s_jit_nent;
static uint32_t s_jit_seen[kJitSeen];     /* block starts reached once */
static uint8_t s_jit_memsel;
static bool s_jit_full;
uint32_t g_jit816_prev;                   /* pc24 of the last instruction a block ran */
uint32_t g_jit816_ended[256];             /* by opcode: translations that stopped at it (no code for it yet) */
uint32_t g_jit816_stats[6];               /* blocks, instructions translated, runs, instructions run, flushes, refused */
static CpuState *s_jit_cpu;
static Interp816 *s_jit_in;
typedef uint32_t (*JitEnterFn)(void *code, Interp816 *in);

enum { JZERO = 0, JAT = 1, JV0 = 2, JV1 = 3, JA0 = 4, JA1 = 5, JA2 = 6, JA3 = 7, JT0 = 8, JT1 = 9, JT2 = 10,
       JT3 = 11, JT4 = 12, JT5 = 13, JS0 = 16, JS1 = 17, JS2 = 18, JS3 = 19, JS4 = 20, JSP = 29, JRA = 31 };
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

static void jit816_flush(void) {
    memset(s_jit_ent, 0, sizeof s_jit_ent);
    memset(s_jit_seen, 0, sizeof s_jit_seen);
    s_jit_nent = 0;
    s_jit_n = 0;
    s_jit_full = false;
    s_jit_memsel = g_memsel;
    g_jit816_stats[4]++;
    /* The way in and out, shared by every block. */
    enum { FRAME = 96 };
    J_ADDIU(JSP, JSP, -FRAME);
    J_SD(JRA, 88, JSP); J_SD(JS0, 80, JSP); J_SD(JS1, 72, JSP); J_SD(JS2, 64, JSP);
    J_SD(JS3, 56, JSP); J_SD(JS4, 48, JSP);
    J_MOVE(JS0, JA1);
    J_JR(JA0);
    J_MOVE(JS1, JZERO);
    s_jit_epilogue = s_jit_n;
    J_MOVE(JV0, JS1);
    J_LD(JRA, 88, JSP); J_LD(JS0, 80, JSP); J_LD(JS1, 72, JSP); J_LD(JS2, 64, JSP);
    J_LD(JS3, 56, JSP); J_LD(JS4, 48, JSP);
    J_JR(JRA);
    J_ADDIU(JSP, JSP, FRAME);
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
        if (e->key == key) return;
        if (!e->key) { e->key = key; e->code = code; e->pg = pg; e->n = n; s_jit_nent++; return; }
    }
}

/* One translated instruction's description. */
enum { JM_NONE, JM_IMM, JM_DP, JM_ABS, JM_ABX, JM_ABY, JM_REL };
enum { JO_NONE, JO_LDA, JO_LDX, JO_LDY, JO_STA, JO_STX, JO_STY, JO_STZ, JO_AND, JO_ORA, JO_EOR, JO_CMP,
       JO_CPX, JO_CPY, JO_INX, JO_INY, JO_DEX, JO_DEY, JO_TAX, JO_TAY, JO_TXA, JO_TYA, JO_CLC, JO_SEC,
       JO_BR, JO_BRA };
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
    };
    JitOp r = { JO_NONE, JM_NONE };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].opc == opc) { r.op = k[i].op; r.mode = k[i].mode; break; }
    return r;
}
static bool jit_op_x(int op) {   /* width from X (index registers) rather than M */
    return op == JO_LDX || op == JO_LDY || op == JO_STX || op == JO_STY || op == JO_CPX || op == JO_CPY;
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
static void jit_exit(uint16_t pc, uint32_t last_pc24) {
    jli(JT0, pc);
    J_SH(JT0, (int)offsetof(Interp816, pc), JS0);
    jli(JT0, last_pc24);
    const int lo = jaddr(&g_jit816_prev);
    jj(s_jit_epilogue);
    J_SW(JT0, lo, JAT);
}

/* Translate the block that starts at pc24 (page pg, widths mf/xf). Returns
 * the number of instructions translated (0: none). */
static int jit_translate(uint32_t pc24, const uint8_t *pg, bool mf, bool xf) {
    const uint32_t mx = (mf ? 2u : 0u) | (xf ? 1u : 0u);
    const uint32_t bank = pc24 & 0xFF0000u;
    uint16_t ipc[kJitMaxInsns + 1];
    uint8_t ilen[kJitMaxInsns];
    JitOp iop[kJitMaxInsns];
    int n = 0;
    uint32_t pc = pc24 & 0xFFFFu;
    const uint32_t page_end = (pc | 0x1FFFu) + 1;
    while (n < kJitMaxInsns) {
        const uint8_t opc = pg[pc & 0x1FFFu];
        const JitOp o = jit_decode(opc);
        if (o.op == JO_NONE || k_burst_stop[opc]) { g_jit816_ended[opc]++; break; }
        const bool byte = jit_op_x(o.op) ? xf : mf;
        const int len = o.mode == JM_NONE ? 1 : o.mode == JM_IMM ? (byte ? 2 : 3) :
                        (o.mode == JM_DP || o.mode == JM_REL) ? 2 : 3;
        if (pc + (uint32_t)len > page_end) break;
#ifdef N64_STATE_AT_PC
        if ((bank | pc) == (uint32_t)(N64_STATE_AT_PC)) break;
#endif
        ipc[n] = (uint16_t)pc; ilen[n] = (uint8_t)len; iop[n] = o;
        n++;
        pc += (uint32_t)len;
        if (o.op == JO_BRA) break;
    }
    if (!n) return 0;
    ipc[n] = (uint16_t)pc;

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
        const bool data = o.mode == JM_DP || o.mode == JM_ABS || o.mode == JM_ABX || o.mode == JM_ABY;
        int cyc = cyclesPerOpcode[opc];
        int accesses = ilen[i];
        if (data || o.mode == JM_IMM) {
            if (!byte) cyc++;                     /* 16-bit: 1 extra cycle */
            if (data) accesses += byte ? 1 : 2;
        }
        const int internal = cyc > accesses ? cyc - accesses : 0;
        const uint32_t fixed = fetch + (uint32_t)internal * 6u;
        const uint32_t last_adr = bank | ((ipc[i] + ilen[i] - 1u) & 0xFFFFu);
        const uint8_t last_byte = pg[(ipc[i] + ilen[i] - 1u) & 0x1FFFu];

        if (o.op == JO_BR || o.op == JO_BRA) {
            /* The flag, taken when it equals bit 5 of the opcode. */
            const uint16_t target = (uint16_t)(ipc[i] + 2 + (int8_t)b1);
            int tl = -1;
            if (target > ipc[i])
                for (int j = i + 1; j <= n; j++) if (ipc[j] == target) tl = j;
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
            if (tl >= 0 && tl < n) {
                const int lo = jaddr(&s_lean_event); J_LD(JT3, lo, JAT); J_SLTU(JT4, JT1, JT3);
                jbr(5, JT4, JZERO, label[tl]); J_NOP();
            }
            jit_latch(last_adr, last_byte);
            jit_exit(target, ia);
            /* the not-taken path's event exit */
            jbind(lexit[i]);
            jit_latch(last_adr, last_byte);
            jit_exit(ipc[i + 1], ia);
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
            default: /* SEC */ J_ORI(JT0, JZERO, 1); J_MTC1(JT0, FC); break;
            }
        } else {
            /* The operand into S2 (a load) or the address into S2 (S3: its
             * high byte's address). */
            if (o.mode == JM_IMM) {
                J_ORI(JS2, JZERO, byte ? b1 : (b1 | b2 << 8));
            } else {
                const uint32_t adr = o.mode == JM_DP ? b1 : (uint32_t)(b1 | b2 << 8);
                if (o.mode == JM_DP) {
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
                        if (!jit_op_store(o.op)) {
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
                /* The accesses add their own speed to s_interp_bus_master. */
                { const int lo = jaddr(&s_interp_bus_master); J_SD(JZERO, lo, JAT); }
                jit_latch(last_adr, last_byte);
                if (jit_op_store(o.op)) {
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
                } else {
                    if (byte) { jjal(jit_rd8); J_MOVE(JA0, JS2); }
                    else { J_MOVE(JA1, JS3); jjal(jit_rd16); J_MOVE(JA0, JS2); }
                    J_MOVE(JS2, JV0);
                }
                { const int lo = jaddr(&s_interp_bus_master); J_LD(JT0, lo, JAT); J_ADDU(JS4, JS4, JT0); }
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
            default: break;   /* stores */
            }
        }
        /* The step's end: clock, SPC pending time, count, event. */
        J_DMFC1(JT1, FMC); J_NOP(); J_DADDU(JT1, JT1, JS4); J_DMTC1(JT1, FMC); J_DMTC1(JT1, FCMC);
        { const int lo = jaddr(&s_apu_pending_master); J_LD(JT2, lo, JAT); J_DADDU(JT2, JT2, JS4); J_SD(JT2, lo, JAT); }
        J_ADDIU(JS1, JS1, 1);
        { const int lo = jaddr(&s_lean_event); J_LD(JT3, lo, JAT); J_SLTU(JT4, JT1, JT3);
          jbr(5, JT4, JZERO, label[i + 1]); J_NOP(); }
        jbind(lexit[i]);
        if (!data) jit_latch(last_adr, last_byte);
        jit_exit(ipc[i + 1], ia);
    }
    /* Past the last instruction: back to the interpreter. */
    jbind(label[n]);
    {
        const uint32_t la = bank | ((ipc[n - 1] + ilen[n - 1] - 1u) & 0xFFFFu);
        const JitOp lo_ = iop[n - 1];
        const bool ldata = lo_.mode == JM_DP || lo_.mode == JM_ABS || lo_.mode == JM_ABX || lo_.mode == JM_ABY;
        if (!ldata) jit_latch(la, pg[la & 0x1FFFu]);
        jit_exit(ipc[n], bank | ipc[n - 1]);
    }
    jfix_all();
    if (s_jit_full) return -1;
    for (int i = 0; i < n; i++)
        jit_add(0x80000000u | mx << 24 | bank | ipc[i], at[i], pg, (uint32_t)(n - i));
    g_jit816_stats[0]++;
    g_jit816_stats[1] += (uint32_t)n;
    return n;
}

/* Run translated code at pc24 if there is any (making it on the second
 * visit). Returns the instructions run; 0 = interpret this one. room = how
 * many instructions the step cap still allows. */
static uint32_t jit816_run(CpuState *cpu, Interp816 *in, uint32_t pc24, const uint8_t *pg, long room) {
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
    if (e->pg != pg || (long)e->n >= room) return 0;
    s_jit_cpu = cpu; s_jit_in = in;
    const uint32_t ran = ((JitEnterFn)(void *)&s_jit_code[0])(&s_jit_code[e->code], in);
    g_jit816_stats[2]++;
    g_jit816_stats[3] += ran;
    return ran;
}
