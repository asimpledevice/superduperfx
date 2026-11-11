/* SPC700 RSP overlay, libdragon side: one run over an SpcRspState block. */
#include <libdragon.h>
#include <stddef.h>
#include "spc_rsp.h"
#include "spc_rsp_layout.h"

DEFINE_RSP_UCODE(rsp_spc);

_Static_assert(sizeof(SpcRspBlock) == SPCST_SIZE, "SpcRspState");
_Static_assert(offsetof(SpcRspBlock, psw) == SPCST_PSW && offsetof(SpcRspBlock, aram) == SPCST_ARAM &&
               offsetof(SpcRspBlock, exit) == SPCST_EXIT && offsetof(SpcRspBlock, ccl) == SPCST_CCL &&
               offsetof(SpcRspBlock, romReadable) == SPCST_ROMREADABLE &&
               offsetof(SpcRspBlock, inPorts) == SPCST_INPORTS && offsetof(SpcRspBlock, outPorts) == SPCST_OUTPORTS &&
               offsetof(SpcRspBlock, timer) == SPCST_TIMERS && offsetof(SpcRspBlock, cycles) == SPCST_CYCLES &&
               offsetof(SpcRspBlock, timerSync) == SPCST_TSYNC && offsetof(SpcRspBlock, dspRegs) == SPCST_DSPREGS,
               "SpcRspState offsets");

static uint32_t s_ovl;

void spc_rsp_init(void) { s_ovl = rspq_overlay_register(&rsp_spc); }

int spc_rsp_exec(SpcRspBlock *b, uint8_t *aram) {
  b->aram = PhysicalAddr(aram);
  data_cache_hit_writeback_invalidate(b, sizeof *b);
  data_cache_hit_writeback_invalidate(aram, 0x10000);
  rspq_write(s_ovl, 0, PhysicalAddr(b));
  rspq_wait();
  return (int)b->exit;
}
