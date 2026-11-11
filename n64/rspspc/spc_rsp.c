/* SPC700 RSP overlay, libdragon side: one run over an SpcRspState block. */
#include <libdragon.h>
#include <stddef.h>
#include <malloc.h>
#include <string.h>
#include "spc_rsp.h"
#include "spc_rsp_layout.h"

DEFINE_RSP_UCODE(rsp_spc);
DEFINE_RSP_UCODE(rsp_spc_ext);   /* runs in the rspq IMEM it borrows (rsp_spc_ext.S) */

_Static_assert(sizeof(SpcRspBlock) == SPCST_SIZE, "SpcRspState");
_Static_assert(offsetof(SpcRspBlock, psw) == SPCST_PSW && offsetof(SpcRspBlock, aram) == SPCST_ARAM &&
               offsetof(SpcRspBlock, exit) == SPCST_EXIT && offsetof(SpcRspBlock, ccl) == SPCST_CCL &&
               offsetof(SpcRspBlock, romReadable) == SPCST_ROMREADABLE &&
               offsetof(SpcRspBlock, inPorts) == SPCST_INPORTS && offsetof(SpcRspBlock, outPorts) == SPCST_OUTPORTS && offsetof(SpcRspBlock, ext) == SPCST_EXT &&
               offsetof(SpcRspBlock, timer) == SPCST_TIMERS && offsetof(SpcRspBlock, cycles) == SPCST_CYCLES &&
               offsetof(SpcRspBlock, timerSync) == SPCST_TSYNC && offsetof(SpcRspBlock, dspRegs) == SPCST_DSPREGS,
               "SpcRspState offsets");

static uint32_t s_ovl;
static uint8_t *s_ext;   /* rsp_spc_ext's code, then the save area for rspq's IMEM */
#define EXT_SAVE_OFS 0x280

void spc_rsp_init(void) {
  s_ovl = rspq_overlay_register(&rsp_spc);
  const size_t n = (size_t)((uint8_t *)rsp_spc_ext.code_end - (uint8_t *)rsp_spc_ext.code);
  assertf(n <= EXT_SAVE_OFS, "rsp_spc_ext: %u bytes", (unsigned)n);
  s_ext = memalign(16, 2 * EXT_SAVE_OFS);
  memset(s_ext, 0, 2 * EXT_SAVE_OFS);
  memcpy(s_ext, rsp_spc_ext.code, n);
  data_cache_hit_writeback_invalidate(s_ext, 2 * EXT_SAVE_OFS);
}

int spc_rsp_exec(SpcRspBlock *b, uint8_t *aram) {
  b->aram = PhysicalAddr(aram);
  b->ext = PhysicalAddr(s_ext);
  data_cache_hit_writeback_invalidate(b, sizeof *b);
  data_cache_hit_writeback_invalidate(aram, 0x10000);
  rspq_write(s_ovl, 0, PhysicalAddr(b));
  rspq_wait();
  return (int)b->exit;
}
