/* SPC700 on the RSP (rsp_spc.S). Plain types only: spc_rsp.c is the
 * libdragon side, spc_rsp_run.c the side that knows the C core. */
#ifndef SPC_RSP_H
#define SPC_RSP_H
#include <stdint.h>

/* SpcRspState (spc_rsp_layout.h). */
typedef struct __attribute__((aligned(16))) {
  uint8_t a, x, y, sp;
  uint16_t pc;
  uint8_t psw, stopped;            /* psw: N V P B H I Z C */
  uint32_t aram;                   /* filled by spc_rsp_exec */
  int32_t budget;
  uint32_t used;
  uint8_t logging, pad3[3];        /* a DSP batch is open */
  uint32_t exit;
  uint8_t ccl, dspAdr, romReadable, pad0;
  uint8_t inPorts[6], outPorts[4], pad1[2];
  uint32_t pad2;
  struct { uint8_t cycles, divider, target, counter, enabled, pad[3]; } timer[3];   /* pad[0] shift, pad[1] undo */
  uint32_t cycles, timerSync;
  uint8_t dspRegs[128];
  uint8_t sync[32];                /* echo pages: reads trap while logging */
  uint32_t rawBuf, undoFp, rawN;   /* raw log: RDRAM address and bytes written; undoFp: SPCST_UNDOFP */
  uint16_t dirPage, pad4;
} SpcRspBlock;

/* The block and the run's area after it (spc_rsp_layout.h SPCRUN_*), as the
 * RSP moves them in and out (SPCRUN_IO bytes). */
typedef struct __attribute__((aligned(16))) {
  SpcRspBlock b;
  uint8_t stage[64];
  uint8_t sp0, psw0, pad[14];
  uint16_t tags[32];               /* line number, bit 15 dirty */
  uint8_t wpages[32];              /* pages of dirty lines evicted during the run */
} SpcRspRun;

void spc_rsp_init(void);
uint32_t spc_rsp_ticks(void);   /* CPU timer (TICKS_READ) */
/* The raw log buffer (SPC_RSP_RAW_BYTES; the CPU reads it after a run). */
#define SPC_RSP_RAW_BYTES (2048 * 8)
void *spc_rsp_raw_log(void);
/* One RSP run over the 64 KB of ARAM (8-byte aligned). Returns SPCEXIT_*. */
int spc_rsp_exec(SpcRspBlock *b, uint8_t *aram);
/* The same in two halves: post the run, and later collect it (waits if the
 * RSP has not finished). Between them the CPU must not touch the block, the
 * raw log or ARAM. */
#include <stdbool.h>
void spc_rsp_post(SpcRspBlock *b, uint8_t *aram);
bool spc_rsp_done(void);
int spc_rsp_wait(SpcRspBlock *b);
extern uint32_t g_spc_rsp_wait_ticks;

struct Apu;
/* As apu_runCycles(apu, clocks), with the SPC on the RSP; opcodes and
 * accesses the RSP leaves to the CPU run in C. */
void spc_rsp_run_apu(struct Apu *apu, uint32_t clocks);
/* Asynchronous runs (SPC_RSP_ASYNC): bring the SPC toward APU cycle
 * `target` (portClock time) without waiting: collect the run in flight if it
 * has finished, then post the next one. Nothing happens while a run is still
 * going, or when less than SPC_RSP_MIN_RUN cycles are owed. */
void spc_rsp_lane_advance(struct Apu *apu, uint64_t target);
/* Collect the run in flight, if any (every reader of SPC state calls this
 * first: RtlApuLock, apu_dspSync). */
void spc_rsp_lane_join(struct Apu *apu);
/* Green's service (blue_set_green_service): collect a finished run and post
 * the next toward the CPU's current time. */
void spc_rsp_lane_pump(void);
void spc_rsp_arm_doorbell(void);
extern uint32_t g_spc_rsp_lane_pumps;
/* The DSP register file the SPC reads back (the host provides it). */
uint8_t *spc_rsp_dsp_regs(struct Apu *apu);

extern uint32_t g_spc_rsp_runs, g_spc_rsp_traps, g_spc_rsp_insns;
#endif
