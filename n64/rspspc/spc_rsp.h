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
  struct { uint8_t cycles, divider, target, counter, enabled, pad[3]; } timer[3];
  uint32_t cycles, timerSync;
  uint8_t dspRegs[128];
  uint8_t sync[32];                /* echo pages: reads trap while logging */
  uint32_t rawBuf, rawCap, rawN;   /* raw log: RDRAM address, room and bytes written */
  uint16_t dirPage, pad4;
} SpcRspBlock;

void spc_rsp_init(void);
/* The raw log buffer (SPC_RSP_RAW_BYTES; the CPU reads it after a run). */
#define SPC_RSP_RAW_BYTES (2048 * 8)
void *spc_rsp_raw_log(void);
/* One RSP run over the 64 KB of ARAM (8-byte aligned). Returns SPCEXIT_*. */
int spc_rsp_exec(SpcRspBlock *b, uint8_t *aram);

struct Apu;
/* As apu_runCycles(apu, clocks), with the SPC on the RSP; opcodes and
 * accesses the RSP leaves to the CPU run in C. */
void spc_rsp_run_apu(struct Apu *apu, uint32_t clocks);
/* The DSP register file the SPC reads back (the host provides it). */
uint8_t *spc_rsp_dsp_regs(struct Apu *apu);

extern uint32_t g_spc_rsp_runs, g_spc_rsp_traps, g_spc_rsp_insns;
#endif
