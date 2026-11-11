/* SPC700 on the RSP, C-core side: converts spc.c's state, runs trapped
 * opcodes in C. */
#include "spc.h"
#include "spc_rsp.h"
#include "spc_rsp_layout.h"

uint32_t g_spc_rsp_runs, g_spc_rsp_traps, g_spc_rsp_insns;

int spc_rsp_run(Spc *spc, int budget) {
  int used = 0;
  while (used < budget) {
    SpcRspRegs r = {spc->a, spc->x, spc->y, spc->sp, spc->pc,
                    (uint8_t)(spc->n << 7 | spc->v << 6 | spc->p << 5 | spc->b << 4 | spc->h << 3 |
                              spc->i << 2 | spc->z << 1 | spc->c),
                    spc->stopped};
    int u; uint32_t n;
    const int exit = spc_rsp_exec(&r, spc->apu->ram, budget - used, &u, &n);
    spc->a = r.a; spc->x = r.x; spc->y = r.y; spc->sp = r.sp; spc->pc = r.pc;
    spc->n = r.psw & 0x80; spc->v = r.psw & 0x40; spc->p = r.psw & 0x20; spc->b = r.psw & 0x10;
    spc->h = r.psw & 8; spc->i = r.psw & 4; spc->z = r.psw & 2; spc->c = r.psw & 1;
    spc->stopped = r.stopped;
    used += u;
    g_spc_rsp_runs++; g_spc_rsp_insns += n;
    if (exit == SPCEXIT_TRAP) { used += spc_runOpcode(spc); g_spc_rsp_traps++; }
  }
  return used;
}
