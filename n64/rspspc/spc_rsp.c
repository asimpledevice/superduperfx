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
               offsetof(SpcRspBlock, inPorts) == SPCST_INPORTS && offsetof(SpcRspBlock, outPorts) == SPCST_OUTPORTS &&
               offsetof(SpcRspBlock, timer) == SPCST_TIMERS && offsetof(SpcRspBlock, cycles) == SPCST_CYCLES &&
               offsetof(SpcRspBlock, timerSync) == SPCST_TSYNC && offsetof(SpcRspBlock, dspRegs) == SPCST_DSPREGS &&
               offsetof(SpcRspBlock, logging) == SPCST_LOGGING && offsetof(SpcRspBlock, sync) == SPCST_SYNC &&
               offsetof(SpcRspBlock, rawBuf) == SPCST_RAWBUF && offsetof(SpcRspBlock, undoFp) == SPCST_UNDOFP && offsetof(SpcRspBlock, rawN) == SPCST_RAWN &&
               offsetof(SpcRspBlock, dirPage) == SPCST_DIRPAGE,
               "SpcRspState offsets");

static uint32_t s_ovl;
static uint8_t *s_ext;   /* rsp_spc_ext's code, then the save area for rspq's IMEM */
#define EXT_SAVE_OFS 0x280   /* then rspq's IMEM (0x278 bytes), then its DMEM at 0x500 */
#define EXT_BUF_SIZE 0x700

void spc_rsp_init(void) {
  s_ovl = rspq_overlay_register(&rsp_spc);
  const size_t n = (size_t)((uint8_t *)rsp_spc_ext.code_end - (uint8_t *)rsp_spc_ext.code);
  assertf(n <= EXT_SAVE_OFS, "rsp_spc_ext: %u bytes", (unsigned)n);
  s_ext = memalign(16, EXT_BUF_SIZE);
  memset(s_ext, 0, EXT_BUF_SIZE);
  memcpy(s_ext, rsp_spc_ext.code, n);
  data_cache_hit_writeback_invalidate(s_ext, EXT_BUF_SIZE);
}

void *spc_rsp_raw_log(void) {   /* RDRAM buffer the RSP writes its raw log to */
  static void *s_raw;
  if (!s_raw) s_raw = memalign(16, SPC_RSP_RAW_BYTES);
  return s_raw;
}

uint32_t g_spc_rsp_exec_ticks, g_spc_rsp_hook_ticks;
uint32_t spc_rsp_ticks(void) { return TICKS_READ(); }

int spc_rsp_exec(SpcRspBlock *b, uint8_t *aram) {
  const uint32_t t0 = TICKS_READ();
  b->aram = PhysicalAddr(aram);
  if (b->logging) b->rawBuf = PhysicalAddr(spc_rsp_raw_log());
  /* The CPU caches ARAM lines between runs (C steps, the DSP's page copies):
   * writing back and dropping the whole 8 KB D-cache (512 index ops) costs
   * less than 4096 hit ops over the 64 KB, and covers the block and the raw
   * log too. The CPU touches none of them until rspq_wait returns. */
  data_cache_writeback_invalidate_all();
  rspq_write(s_ovl, 0, PhysicalAddr(b), PhysicalAddr(s_ext));
  rspq_wait();
  g_spc_rsp_exec_ticks += TICKS_READ() - t0;
  return (int)b->exit;
}
