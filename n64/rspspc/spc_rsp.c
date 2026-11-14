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
#define EXT_SAVE_OFS 0x300   /* then rspq's IMEM, then its borrowed DMEM at 0x600 (rsp_spc.S) */
#define EXT_BUF_SIZE 0x840

#if SPC_RSP_ASYNC
static void on_syncpoint(void);
#endif
void spc_rsp_init(void) {
  s_ovl = rspq_overlay_register(&rsp_spc);
#if SPC_RSP_ASYNC
  rspq_set_syncpoint_hook(on_syncpoint);
#endif
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

static rspq_syncpoint_t s_sp;
uint32_t g_spc_rsp_wait_ticks;

void spc_rsp_post(SpcRspBlock *b, uint8_t *aram) {
  const uint32_t t0 = TICKS_READ();
  b->aram = PhysicalAddr(aram);
  if (b->logging) b->rawBuf = PhysicalAddr(spc_rsp_raw_log());
  /* The CPU caches ARAM lines between runs (C steps, the DSP's page copies):
   * writing back and dropping the whole 8 KB D-cache (512 index ops) costs
   * less than 4096 hit ops over the 64 KB, and covers the block and the raw
   * log too. The CPU touches none of them until the run is collected
   * (spc_rsp_wait): every reader joins first (spc_rsp_run.c). */
  data_cache_writeback_invalidate_all();
  rspq_write(s_ovl, 0, PhysicalAddr(b), PhysicalAddr(s_ext));
  s_sp = rspq_syncpoint_new();
  rspq_flush();
  g_spc_rsp_exec_ticks += TICKS_READ() - t0;
}

bool spc_rsp_done(void) { return rspq_syncpoint_check(s_sp); }

#if SPC_RSP_ASYNC
#include "run_flags.h"
/* The doorbell: when a posted run finishes, ask Blue to yield to Green at
 * its next safe point, where Green's service collects it and posts the next
 * (spc_rsp_lane_pump), so the RSP is not left idle until the bridge's next
 * catch-up. */
static volatile bool s_bell_armed;
static void on_syncpoint(void) {
  if (s_bell_armed && rspq_syncpoint_check(s_sp)) {
    s_bell_armed = false;
    run_flags_set_in_interrupt(RUN_FLAG_WANT_GREEN);
  }
}
void spc_rsp_arm_doorbell(void) { s_bell_armed = true; }
#endif

int spc_rsp_wait(SpcRspBlock *b) {
  if (!rspq_syncpoint_check(s_sp)) {
    const uint32_t t0 = TICKS_READ();
    rspq_syncpoint_wait(s_sp);
    g_spc_rsp_wait_ticks += TICKS_READ() - t0;
  }
  return (int)b->exit;
}

int spc_rsp_exec(SpcRspBlock *b, uint8_t *aram) {
  spc_rsp_post(b, aram);
  return spc_rsp_wait(b);
}
