/* SPC700 on the RSP (rsp_spc.S). Plain types only: spc_rsp.c is the
 * libdragon side, spc_rsp_run.c the side that knows the C core. */
#ifndef SPC_RSP_H
#define SPC_RSP_H
#include <stdint.h>

typedef struct {
  uint8_t a, x, y, sp;
  uint16_t pc;
  uint8_t psw, stopped;   /* psw: N V P B H I Z C */
} SpcRspRegs;

void spc_rsp_init(void);
/* One RSP run of up to `budget` cycles over the 64 KB of ARAM (8-byte
 * aligned). Returns SPCEXIT_*; *used and *count are the cycles and
 * instructions it ran. */
int spc_rsp_exec(SpcRspRegs *r, uint8_t *aram, int budget, int *used, uint32_t *count);

struct Spc;
/* Run until at least `budget` cycles are used, as repeated spc_runOpcode
 * calls would; trapped opcodes run in C. Returns the cycles used. */
int spc_rsp_run(struct Spc *spc, int budget);

extern uint32_t g_spc_rsp_runs, g_spc_rsp_traps, g_spc_rsp_insns;
#endif
