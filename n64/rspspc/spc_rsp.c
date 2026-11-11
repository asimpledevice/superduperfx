/* SPC700 RSP overlay, libdragon side: one run over an SpcRspState block. */
#include <libdragon.h>
#include "spc_rsp.h"
#include "spc_rsp_layout.h"

DEFINE_RSP_UCODE(rsp_spc);

typedef struct __attribute__((aligned(16))) {
  SpcRspRegs r;
  uint32_t aram;
  int32_t budget;
  uint32_t used, count, exit, pad;
} SpcRspBlock;
_Static_assert(sizeof(SpcRspBlock) == SPCST_SIZE, "SpcRspState");
_Static_assert(offsetof(SpcRspBlock, r.psw) == SPCST_PSW && offsetof(SpcRspBlock, aram) == SPCST_ARAM &&
               offsetof(SpcRspBlock, exit) == SPCST_EXIT, "SpcRspState offsets");

static SpcRspBlock s_blk;
static uint32_t s_ovl;

void spc_rsp_init(void) { s_ovl = rspq_overlay_register(&rsp_spc); }

int spc_rsp_exec(SpcRspRegs *r, uint8_t *aram, int budget, int *used, uint32_t *count) {
  s_blk.r = *r;
  s_blk.aram = PhysicalAddr(aram);
  s_blk.budget = budget;
  data_cache_hit_writeback_invalidate(&s_blk, sizeof s_blk);
  data_cache_hit_writeback_invalidate(aram, 0x10000);
  rspq_write(s_ovl, 0, PhysicalAddr(&s_blk));
  rspq_wait();
  *r = s_blk.r;
  *used = (int)s_blk.used;
  *count = s_blk.count;
  return (int)s_blk.exit;
}
