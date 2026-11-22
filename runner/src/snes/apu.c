
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


#include "../time_cats.h"
#include "apu.h"
#include "snes.h"
#include "spc.h"
#include "dsp.h"
#include "apu_dsp_watch.h"

#if SNESRECOMP_APU_DEFER_DSP
static inline void apu_dspJoin(Apu* apu);
/* The SPC translator caches which blocks may run against the page marks:
 * a new sync mark starts a new epoch (apu_dspMark; n64/src/spc_dbt.c). */
#if SNESRECOMP_SPC_DBT
#define SPC_DBT_MARKS_CHANGED() (g_spc_dbt_epoch += 2)   /* (bit 0 follows P: spc.c) */
#else
#define SPC_DBT_MARKS_CHANGED() ((void)0)
#endif
#endif
#if SNESRECOMP_DSP_FRONTEND
static void apu_feNow(Apu* apu);
#endif
#include "../audio_trace.h"

#ifndef SNESRECOMP_TRACE
#define SNESRECOMP_TRACE 0
#endif

#ifndef SNESRECOMP_SPC_DIAGNOSTICS
#define SNESRECOMP_SPC_DIAGNOSTICS SNESRECOMP_TRACE
#endif

#ifndef SNESRECOMP_APU_NATIVE
#define SNESRECOMP_APU_NATIVE 0
#endif
#if SNESRECOMP_APU_NATIVE != 0 && SNESRECOMP_APU_NATIVE != 1
#error SNESRECOMP_APU_NATIVE must be 0 or 1
#endif

static const uint8_t bootRom[0x40] = {
  0xcd, 0xef, 0xbd, 0xe8, 0x00, 0xc6, 0x1d, 0xd0, 0xfc, 0x8f, 0xaa, 0xf4, 0x8f, 0xbb, 0xf5, 0x78,
  0xcc, 0xf4, 0xd0, 0xfb, 0x2f, 0x19, 0xeb, 0xf4, 0xd0, 0xfc, 0x7e, 0xf4, 0xd0, 0x0b, 0xe4, 0xf5,
  0xcb, 0xf4, 0xd7, 0x00, 0xfc, 0xd0, 0xf3, 0xab, 0x01, 0x10, 0xef, 0x7e, 0xf4, 0x10, 0xeb, 0xba,
  0xf6, 0xda, 0x00, 0xba, 0xf4, 0xc4, 0xf4, 0xdd, 0x5d, 0xd0, 0xdb, 0x1f, 0x00, 0x00, 0xc0, 0xff
};

Apu* apu_init(void) {
  Apu* apu = calloc(1, sizeof(Apu));  /* zero padding: saveload/co-sim hash determinism */
  apu->spc = spc_init(apu);
  apu->dsp = dsp_init(apu->ram);
  apu_clearPortQueue(apu);
  return apu;
}

void apu_free(Apu* apu) {
  spc_free(apu->spc);
  dsp_free(apu->dsp);
  free(apu);
}

void apu_reset(Apu* apu) {
#if SNESRECOMP_APU_DEFER_DSP
  apu_dspJoin(apu);
#endif
#if SNESRECOMP_SPC_DBT
  spc_dbt_flush();   /* ARAM is about to change wholesale */
  spc_dbt_reset_volatile();
#endif
  apu->romReadable = true; // before resetting spc, because it reads reset vector from it
  spc_reset(apu->spc);
  dsp_reset(apu->dsp);
#if SNESRECOMP_APU_DEFER_DSP
  memcpy(apu->dspRegs, apu->dsp->ram, sizeof(apu->dspRegs));
#endif
  memset(apu->ram, 0, sizeof(apu->ram));
  apu_dspViewStale(apu);
  apu->dspAdr = 0;
  apu->cycles = 0;
  apu->timerSyncCycle = 0;
#if SNESRECOMP_APU_DEFER_DSP
  apu->dspPending = 0;  /* owed samples belong to the machine being reset */
  apu->dspLogN = 0;
#endif
  memset(apu->inPorts, 0, sizeof(apu->inPorts));
  memset(apu->outPorts, 0, sizeof(apu->outPorts));
  for(int i = 0; i < 3; i++) {
    apu->timer[i].cycles = 0;
    apu->timer[i].divider = 0;
    apu->timer[i].target = 0;
    apu->timer[i].counter = 0;
    apu->timer[i].enabled = false;
  }
  apu->cpuCyclesLeft = 7;
  apu->portClock = 0;
  apu_clearPortQueue(apu);
#if SNESRECOMP_DSP_FRONTEND
  apu->dspEdges = 0;
  apu->feValid = false;   /* rebuilt from the reset Dsp at its next use */
#if SNESRECOMP_ECHO_IN_PLACE
  apu->dspAt = apu->dspFlightEnd = 0;
  memset(apu->echoTouch, 0, sizeof apu->echoTouch);
  memset(apu->echoDirty, 0, sizeof apu->echoDirty);
  apu->echoRuns = 0;
#endif
#if SNESRECOMP_DSP_STREAM
  apu_yellow_discard();   /* the S-DSP's state is the reset Dsp's again */
  memset(apu->echoPageTouch, 0, sizeof apu->echoPageTouch);
  memset(apu->echoPageDirty, 0, sizeof apu->echoPageDirty);
  memset(apu->echoWriteSlow, 0, sizeof apu->echoWriteSlow);
  memset(apu->echoReadSlow, 0, sizeof apu->echoReadSlow);
  memset(apu->echoRegion, 0, sizeof apu->echoRegion);
  memset(apu->dspRegAt, 0, sizeof apu->dspRegAt);
  apu_feNow(apu);         /* the front end tracks from the first sample */
#endif
#endif
}

void apu_clearPortQueue(Apu* apu) {
  apu->portQHead = apu->portQTail = 0;
  apu->portGuestAnchor = 0;
  apu->portTargetAnchor = 0;
  apu->portLastGuest = 0;
  apu->portLastTarget = 0;
  apu->portTimeValid = false;
}

void apu_writePortNow(Apu* apu, uint8_t port, uint8_t val) {
  port &= 3;
  apu->inPorts[port] = val;
  audio_trace_on_cpu_port_apply(port, val);
}

static void apu_applyPortWrite(Apu* apu, const ApuPortWrite *w) {
  apu_writePortNow(apu, w->port, w->val);
}

uint32_t apu_portQueueDepth(const Apu* apu) {
  return apu->portQTail - apu->portQHead;
}

#ifdef N64_PHASE0
/* Measurement counters (n64 N64_PHASE0 builds): CPU port writes scheduled,
 * SPC catch-ups (apu_runCycles), and the most writes between two of them. */
uint32_t g_apu_port_writes, g_apu_port_burst;
uint32_t g_apu_catchups, g_apu_port_burst_max;
#endif

#ifdef N64_STATE_HASH
/* (Check builds: how often a port write re-anchored guest time to the SPC's
 * clock -- [0] no anchor or guest time went back, [1] the SPC was already
 * past the write's time -- and [2] by how many cycles in total.) */
uint32_t g_apu_rebase[3];
#endif
bool apu_schedulePortWrite(Apu* apu, uint8_t port, uint8_t val,
                           uint64_t guest_cycle) {
  if (apu_portQueueDepth(apu) >= APU_PORT_QUEUE_LEN)
    return false;
#ifdef N64_PHASE0
  g_apu_port_writes++;
  g_apu_port_burst++;
#endif

  /* Establish a correspondence between guest time and wherever the
   * callback-driven SPC is now. If the callback later runs ahead, rebase at
   * the new current point; future guest deltas are still preserved. */
  if (!apu->portTimeValid || guest_cycle < apu->portLastGuest) {
    apu->portGuestAnchor = guest_cycle;
    apu->portTargetAnchor = apu->portClock;
    apu->portTimeValid = true;
#ifdef N64_STATE_HASH
    g_apu_rebase[0]++;
#endif
  }

  uint64_t target = apu->portTargetAnchor +
                    (guest_cycle - apu->portGuestAnchor);
  if (target < apu->portClock) {
#ifdef N64_STATE_HASH
    g_apu_rebase[1]++;
    g_apu_rebase[2] += (uint32_t)(apu->portClock - target);
#endif
    apu->portGuestAnchor = guest_cycle;
    apu->portTargetAnchor = apu->portClock;
    target = apu->portClock;
  }
  if (target < apu->portLastTarget)
    target = apu->portLastTarget;

  ApuPortWrite *w = &apu->portQueue[apu->portQTail & (APU_PORT_QUEUE_LEN - 1)];
  w->target_cycle = target;
  w->port = (uint8_t)(port & 3);
  w->val = val;
  apu->portQTail++;
  apu->portLastGuest = guest_cycle;
  apu->portLastTarget = target;
  return true;
}

/* Apply all CPU bus events due before this SPC cycle executes. */
static inline void apu_drainPortQueue(Apu* apu) {
  while (apu->portQHead != apu->portQTail) {
    ApuPortWrite *w = &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1)];
    if (w->target_cycle > apu->portClock)
      break;
    apu_applyPortWrite(apu, w);
    apu->portQHead++;
  }
}

bool apu_runUntilPortQueueEmpty(Apu* apu, uint32_t max_cycles) {
  while (apu->portQHead != apu->portQTail) {
    if (!max_cycles) return false;
    const ApuPortWrite *write =
        &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)];
    uint64_t distance = write->target_cycle > apu->portClock
        ? write->target_cycle - apu->portClock : 0;
    uint32_t span = distance >= max_cycles ? max_cycles : (uint32_t)distance + 1u;
    apu_runCycles(apu, span);
    max_cycles -= span;
  }
  return true;
}

bool apu_guestTarget(const Apu* apu, uint64_t guest_cycle, uint64_t *target) {
  if (!apu->portTimeValid || guest_cycle < apu->portGuestAnchor)
    return false;
  uint64_t t = apu->portTargetAnchor + (guest_cycle - apu->portGuestAnchor);
  if (t < apu->portLastTarget)
    t = apu->portLastTarget;
  *target = t;
  return true;
}

bool apu_runToGuestCycle(Apu* apu, uint64_t guest_cycle,
                         uint32_t max_cycles) {
  uint64_t target;
  if (!apu_guestTarget(apu, guest_cycle, &target))
    return true;

  for (;;) {
    /* Bus writes at the boundary precede that cycle's SPC work. Drain them
     * directly: executing a cycle just to apply a due write advances past the
     * target and makes a second write at the same guest time rebase. */
    apu_drainPortQueue(apu);
    if (apu->portClock >= target)
      return true;
    if (!max_cycles) return false;
    uint64_t distance = target - apu->portClock;
    uint32_t span = distance > max_cycles ? max_cycles : (uint32_t)distance;
    apu_runCycles(apu, span);
    max_cycles -= span;
  }
}

bool apu_waitForTransferReady(Apu* apu, uint8_t request_port,
                              uint8_t request_value, uint32_t max_cycles) {
  for (;;) {
    if (apu->outPorts[0] == 0xaa && apu->outPorts[1] == 0xbb)
      return true;
    if (apu->inPorts[request_port & 3] != request_value)
      apu_writePortNow(apu, request_port, request_value);
    if (max_cycles-- == 0)
      return false;
    apu_cycle(apu);
  }
}

bool apu_finishHleTransfer(Apu* apu, uint16_t final_pc,
                           uint32_t max_cycles) {
  apu_writePortNow(apu, 2, (uint8_t)final_pc);
  apu_writePortNow(apu, 3, (uint8_t)(final_pc >> 8));
  apu_writePortNow(apu, 1, 0);
  apu_writePortNow(apu, 0, 0xcc);
  for (;;) {
    if (apu->outPorts[0] == 0xcc) {
      memset(apu->inPorts, 0, 4);
      return true;
    }
    if (max_cycles-- == 0)
      return false;
    apu_cycle(apu);
  }
}

void apu_port_sched_save(const Apu *apu, ApuPortSched *out) {
  if (!apu || !out) return;
  memcpy(out->queue, apu->portQueue, sizeof(out->queue));
  out->qHead = apu->portQHead;
  out->qTail = apu->portQTail;
  out->clock = apu->portClock;
  out->guestAnchor = apu->portGuestAnchor;
  out->targetAnchor = apu->portTargetAnchor;
  out->lastGuest = apu->portLastGuest;
  out->lastTarget = apu->portLastTarget;
  out->timeValid = apu->portTimeValid;
}

void apu_port_sched_restore(Apu *apu, const ApuPortSched *in) {
  if (!apu || !in) return;
  memcpy(apu->portQueue, in->queue, sizeof(in->queue));
  apu->portQHead = in->qHead;
  apu->portQTail = in->qTail;
  apu->portClock = in->clock;
  apu->portGuestAnchor = in->guestAnchor;
  apu->portTargetAnchor = in->targetAnchor;
  apu->portLastGuest = in->lastGuest;
  apu->portLastTarget = in->lastTarget;
  apu->portTimeValid = in->timeValid;
}

#if SNESRECOMP_APU_DEFER_DSP
/* Sync statistics by reason (see APU_DSP_SYNC_* in apu.h). */
uint32_t g_apu_dsp_sync_count[APU_DSP_SYNC_REASONS];
uint32_t g_apu_dsp_sync_samples[APU_DSP_SYNC_REASONS];

/* Replay the owed samples. ARAM is first returned to its state at the start
 * of the batch (logged writes undone newest first), then each owed sample
 * runs after the writes the SPC made before it, in their original order. */
/* isReg values: 0 ARAM write on a watched page, 1 DSP register write,
 * 2 ARAM write on an unwatched page (verify builds only; page-watch
 * tracking would not have logged it). */
static void apu_dspRevert(Apu* apu, uint32_t n, bool narrow) {
  apu_dspViewStale(apu);  /* the C DSP writes ARAM itself (echo) */
  for (uint32_t i = n; i-- > 0;) {
    const ApuDspLogEntry *e = &apu->dspLog[i];
    if (e->isReg == 0 || (e->isReg == 2 && !narrow)) apu->ram[e->adr] = e->old;
  }
}

static void apu_dspForward(Apu* apu, uint32_t pending, uint32_t n, bool narrow) {
  apu_dspViewStale(apu);
  uint32_t i = 0;
  for (uint32_t k = 0; k <= pending; k++) {
    for (; i < n && apu->dspLog[i].tag == k; i++) {
      const ApuDspLogEntry *e = &apu->dspLog[i];
      if (e->isReg == 1) dsp_write(apu->dsp, (uint8_t)e->adr, e->val);
      else if (e->isReg == 0 || e->isReg == 3 || !narrow) apu->ram[e->adr] = e->val;
    }
    if (k < pending) dsp_cycle(apu->dsp);
  }
}

static void apu_dspReplay(Apu* apu, uint32_t pending, uint32_t n, bool narrow) {
  apu_dspRevert(apu, n, narrow);
  apu_dspForward(apu, pending, n, narrow);
}

uint32_t g_apu_dsp_verify_mismatches;

static void apu_dspMark(Apu* apu, uint16_t start, uint32_t len, uint8_t bits);
static void apu_dspMarkDirEntry(Apu* apu, uint8_t srcn);
static void apu_dspMarkRegWindows(Apu* apu);
#define APU_DSP_BRR_SPAN (65u * 9u)

uint32_t g_apu_dsp_join_why[8];
static unsigned s_join_why;
/* Wait for a batch running elsewhere and take its results. */
static inline void apu_dspJoin(Apu* apu) {
#ifdef SNESRECOMP_DSP_JOIN_HOOK
  if (!apu->dspInFlight) return;
  g_apu_dsp_join_why[s_join_why & 7]++;
  SNESRECOMP_DSP_JOIN_HOOK();
  apu->dspInFlight = false;
  memset(apu->dspFlightWrite, 0, sizeof apu->dspFlightWrite);
#if SNESRECOMP_ECHO_IN_PLACE
  apu->dspAt = apu->dspFlightEnd;
#endif
  if (!apu->dspPending) {  /* marks left for the flight; none owed now */
    memset(apu->spc->dspWatchLog, 0, sizeof(apu->spc->dspWatchLog));
    memset(apu->spc->dspWatchSync, 0, sizeof(apu->spc->dspWatchSync));
#if SNESRECOMP_ECHO_IN_PLACE
    /* Writes diverted since (they go with the next batch). */
    for (uint32_t i = 0; i < apu->dspLogN; i++)
      if (apu->dspLog[i].isReg == 3) {
        const uint16_t a = apu->dspLog[i].adr;
        apu->spc->dspWatchLog[a >> 11] |= (uint8_t)(1u << ((a >> 8) & 7));
        apu->spc->dspWatchSync[a >> 11] |= (uint8_t)(1u << ((a >> 8) & 7));
      }
#endif
  }
#else
  (void)apu;
#endif
}

#ifdef SNESRECOMP_DSP_LAUNCH_HOOK
/* After a launch the Dsp shows the in-flight batch's start state until the
 * join, so mark what the next batch can read from that state conservatively:
 * two batches of BRR data per voice, the directory entries of every SRCN in
 * play, and the echo window (writes logged, reads synced). */
static void apu_dspMarkAfterLaunch(Apu* apu, uint32_t n) {
  const Dsp *d = apu->dsp;
  apu_dspMark(apu, d->dirPage, 0x400u, APU_DSP_WATCH_LOG);
  for (int v = 0; v < 8; v++) {
    apu_dspMark(apu, d->channel[v].decodeOffset, 2u * APU_DSP_BRR_SPAN, APU_DSP_WATCH_LOG);
    apu_dspMarkDirEntry(apu, d->channel[v].srcn);
  }
  for (uint32_t i = 0; i < n; i++)
    if (apu->dspLog[i].isReg && (apu->dspLog[i].adr & 0x0f) == 4)
      apu_dspMarkDirEntry(apu, apu->dspLog[i].val);
  uint32_t loop = d->echoDelay;
  const uint32_t current = (uint32_t)d->echoBufferIndex + d->echoRemain;
  if (current > loop) loop = current;
  apu_dspMark(apu, d->echoBufferAdr, loop * 4u,
              APU_DSP_WATCH_LOG | (d->echoWrites ? APU_DSP_WATCH_SYNC : 0u));
  apu_dspMarkRegWindows(apu);
  /* The flight's own echo pages: SPC writes there join first. */
  for (int i = 0; i < 32; i++) apu->spc->dspWatchLog[i] |= apu->dspFlightWrite[i];
}
#endif

#if SNESRECOMP_DSP_FRONTEND
static void apu_feTakeRecords(Apu* apu);
static void apu_feBatchStart(Apu* apu);
#endif
#if SNESRECOMP_DSP_STREAM
static void apu_streamPost(Apu* apu);
#endif
static void apu_dspSyncFor(Apu* apu, unsigned reason) {
#if SNESRECOMP_DSP_STREAM
  /* The audio split: post what is owed to the S-DSP; never wait. */
  if (!apu->dspPending && !apu->dspLogN) return;
  g_apu_dsp_sync_count[reason]++;
  g_apu_dsp_sync_samples[reason] += apu->dspPending;
  apu_streamPost(apu);
  return;
#endif
#ifdef SNESRECOMP_DSP_JOIN_HOOK
  /* A frame-end flush never waits for a batch still running: its samples
   * stay owed and go with the next batch. (After the frame's last SPC
   * catch-up a full batch is usually just launched; joining it here put
   * the CPU to sleep for the whole batch.) */
  if (reason == APU_DSP_SYNC_FRAME && apu->dspInFlight) return;
#endif
  s_join_why = reason;
  apu_dspJoin(apu);
  const uint32_t pending = apu->dspPending, n = apu->dspLogN;
  if (!pending && !n) return;
  g_apu_dsp_sync_count[reason]++;
  g_apu_dsp_sync_samples[reason] += pending;
  apu->dspPending = 0;
  const bool aramEndReachable = APU_DSP_PAGE_BIT(apu->spc->dspWatchLog, 0xff00u) != 0;
  (void)aramEndReachable;
#ifdef SNESRECOMP_DSP_LAUNCH_HOOK
  uint8_t marks[sizeof(apu->spc->dspWatchLog)], writes[sizeof(apu->spc->dspWatchSync)];
  memcpy(marks, apu->spc->dspWatchLog, sizeof marks);
  memcpy(writes, apu->spc->dspWatchSync, sizeof writes);
#endif
  memset(apu->spc->dspWatchLog, 0, sizeof(apu->spc->dspWatchLog));  /* nothing owed now */
  memset(apu->spc->dspWatchSync, 0, sizeof(apu->spc->dspWatchSync));
#if SNESRECOMP_DSP_FRONTEND
  apu_feTakeRecords(apu);   /* this batch's block and key-on records */
#endif
#if SNESRECOMP_APU_DEFER_DSP_VERIFY
  static uint8_t ram0[0x10000], ramN[0x10000];
  static Dsp dsp0, dspN;
  memcpy(ram0, apu->ram, sizeof ram0);
  dsp0 = *apu->dsp;
  apu_dspReplay(apu, pending, n, true);  /* as page-watch tracking would */
  memcpy(ramN, apu->ram, sizeof ramN);
  dspN = *apu->dsp;
  memcpy(apu->ram, ram0, sizeof ram0);
  *apu->dsp = dsp0;
  apu_dspReplay(apu, pending, n, false);  /* exact: every write logged */
  if (memcmp(ramN, apu->ram, sizeof ramN) ||
      memcmp(dspN.ram, apu->dsp->ram, sizeof(Dsp) - offsetof(Dsp, ram))) {
    if (g_apu_dsp_verify_mismatches++ < 8)
      fprintf(stderr, "[apu] deferred DSP verify: page-watch replay differs (batch %u samples)\n",
              (unsigned)pending);
  }
#elif defined(SNESRECOMP_DSP_LAUNCH_HOOK)
  if (SNESRECOMP_DSP_LAUNCH_HOOK(apu->dsp, apu->ram, apu->dspLog, n, pending, marks,
                                 apu->spc->dspViewFresh)) {
    apu->dspInFlight = true;
    memcpy(apu->dspFlightWrite, writes, sizeof writes);
    apu_dspMarkAfterLaunch(apu, n);
#if SNESRECOMP_ECHO_IN_PLACE
    /* Until the join: every page the batch could touch stays marked (SPC
     * writes there are checked against the window, reads that need it
     * wait). */
    apu->dspFlightEnd = apu->dspEdges;
    for (int i = 0; i < 32; i++) {
      apu->spc->dspWatchLog[i] |= marks[i] | writes[i];
      apu->spc->dspWatchSync[i] |= writes[i];
    }
#endif
    /* Only full batches run in the background; other syncs want results. */
    if (reason != APU_DSP_SYNC_FULL_BATCH && reason != APU_DSP_SYNC_LOG_FULL &&
        reason != APU_DSP_SYNC_FRAME) {
      s_join_why = 6;
      apu_dspJoin(apu);
    }
  } else {
    apu_dspReplay(apu, pending, n, true);
#if SNESRECOMP_ECHO_IN_PLACE
    apu->dspAt = apu->dspEdges;
#endif
  }
#elif defined(SNESRECOMP_DSP_BATCH_HOOK)
  apu_dspRevert(apu, n, true);
  if (!SNESRECOMP_DSP_BATCH_HOOK(apu->dsp, apu->ram, apu->dspLog, n, pending, aramEndReachable))
    apu_dspForward(apu, pending, n, true);
#else
  apu_dspReplay(apu, pending, n, true);
#if SNESRECOMP_ECHO_IN_PLACE
  apu->dspAt = apu->dspEdges;
#endif
#endif
  apu->dspLogN = 0;
}

#if SNESRECOMP_DSP_STREAM
static void apu_streamStop(Apu* apu);
/* The Dsp current (saves, front-end rebuilds, hosts that want it): the
 * S-DSP hands its state back. It takes it again at the next post. */
void apu_dspSync(Apu* apu) { apu_streamStop(apu); }
#else
void apu_dspSync(Apu* apu) { apu_dspSyncFor(apu, APU_DSP_SYNC_HOST); }
#endif
void apu_dspFrameSync(Apu* apu) {
  apu_dspSyncFor(apu, SNESRECOMP_DSP_HOST_ASYNC ? APU_DSP_SYNC_FRAME : APU_DSP_SYNC_HOST);
}
void apu_dspEchoRead(Apu* apu) { apu_dspSyncFor(apu, APU_DSP_SYNC_ECHO_READ); }

/* APU_DSP_BRR_SPAN (above): the most BRR data one voice can consume in a
 * batch: at the maximum pitch (0x3fff) the 16-bit sample counter wraps at
 * most 64 times in 256 samples, each wrap decoding one 9-byte block, plus
 * the block in progress. */
_Static_assert(APU_DSP_MAX_PENDING * 0x3fffu / 0x10000u + 1u <= 65u, "BRR span");

static void apu_dspMark(Apu* apu, uint16_t start, uint32_t len, uint8_t bits) {
  if (!len) return;
  const uint32_t first = start >> 8, last = ((uint32_t)start + len - 1u) >> 8;
  for (uint32_t p = first; p <= last; p++) {
    const uint8_t bit = (uint8_t)(1u << (p & 7));
#if SNESRECOMP_SPC_DBT
    /* Translated blocks were checked against the sync marks (their reads);
     * log marks are checked by every store and clearing marks never makes
     * a block invalid, so only a new sync mark starts an epoch. */
    if ((bits & APU_DSP_WATCH_SYNC) && !(apu->spc->dspWatchSync[(p >> 3) & 31] & bit) &&
        (g_spc_dbt_rpages[(p >> 3) & 31] & bit))   /* (a page no block reads: none to recheck) */
      SPC_DBT_MARKS_CHANGED();
    /* The S-DSP may write this page (echo): translated code on it would go
     * stale without an SPC store to say so. */
    if ((bits & APU_DSP_WATCH_SYNC) && g_spc_dbt_code[p & 255]) spc_dbt_code_page_written((uint16_t)(p << 8));
#endif
    if (bits & APU_DSP_WATCH_LOG) apu->spc->dspWatchLog[(p >> 3) & 31] |= bit;
    if (bits & APU_DSP_WATCH_SYNC) apu->spc->dspWatchSync[(p >> 3) & 31] |= bit;
  }
}

/* Sample data reachable through one directory entry: its start (key-on) and
 * loop addresses, as the entry reads now. */
static void apu_dspMarkDirEntryAt(Apu* apu, uint16_t dir, uint8_t srcn) {
  const uint16_t e = (uint16_t)(dir + 4u * srcn);
  const uint8_t *r = apu->ram;
  apu_dspMark(apu, (uint16_t)(r[e] | (r[(uint16_t)(e + 1)] << 8)), APU_DSP_BRR_SPAN, APU_DSP_WATCH_LOG);
  apu_dspMark(apu, (uint16_t)(r[(uint16_t)(e + 2)] | (r[(uint16_t)(e + 3)] << 8)), APU_DSP_BRR_SPAN,
              APU_DSP_WATCH_LOG);
}
/* The directory as the Dsp has it, and as the register file has it: they
 * differ while a DIR write is logged, or a batch is in flight. */
static void apu_dspMarkDirEntry(Apu* apu, uint8_t srcn) {
  const uint16_t dsp_dir = apu->dsp->dirPage, reg_dir = (uint16_t)(apu->dspRegs[0x5d] << 8);
  apu_dspMarkDirEntryAt(apu, dsp_dir, srcn);
  if (reg_dir != dsp_dir) apu_dspMarkDirEntryAt(apu, reg_dir, srcn);
}

/* FLG, ESA, EDL and DIR writes are logged like any register write, so a
 * batch may see the echo window and the directory move. Marks cover every
 * place they can be: the echo window at the Dsp's ESA and the register
 * file's, over the longest loop either EDL or the loop in progress gives
 * (the DSP takes a new EDL when its loop ends), written when either FLG
 * allows echo writes; the directory and every voice's entry under either
 * DIR, for either SRCN. Called when such a write is logged, and with every
 * batch's marks. */
static void apu_dspMarkRegWindows(Apu* apu) {
  const Dsp *d = apu->dsp;
  const uint8_t *r = apu->dspRegs;
  uint32_t loop = d->echoDelay, edl = (r[0x7d] & 0xfu) * 512u;
  const uint32_t current = (uint32_t)d->echoBufferIndex + d->echoRemain;
  if (current > loop) loop = current;
  if (edl > loop) loop = edl;
  const uint8_t bits = APU_DSP_WATCH_LOG |
                       ((d->echoWrites || !(r[0x6c] & 0x20)) ? APU_DSP_WATCH_SYNC : 0u);
  apu_dspMark(apu, d->echoBufferAdr, loop * 4u, bits);
  apu_dspMark(apu, (uint16_t)(r[0x6d] << 8), loop * 4u, bits);
  const uint16_t reg_dir = (uint16_t)(r[0x5d] << 8);
  apu_dspMark(apu, reg_dir, 0x400u, APU_DSP_WATCH_LOG);
  for (int v = 0; v < 8; v++) {
    apu_dspMarkDirEntry(apu, d->channel[v].srcn);
    apu_dspMarkDirEntry(apu, r[v * 16 + 4]);
  }
}

/* Mark every ARAM page the DSP can read (log SPC writes there) or write
 * (sync before SPC reads there) during the batch. SRCN, directory, FLG, ESA,
 * EDL and DIR writes extend the marks when they make new pages reachable
 * (apu_dspMarkRegWindows). */
static void apu_dspStartBatch(Apu* apu) {
#if SNESRECOMP_DSP_FRONTEND
  apu_feBatchStart(apu);
#endif
#if SNESRECOMP_DSP_STREAM
  return;   /* no marks: the S-DSP reads records, and its echo lines are stamped */
#endif
  const Dsp *d = apu->dsp;
  uint32_t loop = d->echoDelay;
  const uint32_t current = (uint32_t)d->echoBufferIndex + d->echoRemain;
  if (current > loop) loop = current;
  apu_dspMark(apu, d->echoBufferAdr, loop * 4u,
              APU_DSP_WATCH_LOG | (d->echoWrites ? APU_DSP_WATCH_SYNC : 0u));
  apu_dspMark(apu, d->dirPage, 0x400u, APU_DSP_WATCH_LOG);
  for (int v = 0; v < 8; v++) {
    apu_dspMark(apu, d->channel[v].decodeOffset, APU_DSP_BRR_SPAN, APU_DSP_WATCH_LOG);
    apu_dspMarkDirEntry(apu, d->channel[v].srcn);
  }
  apu_dspMarkRegWindows(apu);
}

/* An SPC write to a page the DSP may read during the batch. */
#if SNESRECOMP_ECHO_IN_PLACE
static bool apu_echoDivert(Apu* apu, uint16_t adr, uint8_t val);
#endif
void apu_dspLogRam(Apu* apu, uint16_t adr, uint8_t val) {
#if SNESRECOMP_ECHO_IN_PLACE
  if (apu_echoDivert(apu, adr, val)) return;   /* for the S-DSP to apply */
#else
  if (apu->dspInFlight && APU_DSP_PAGE_BIT(apu->dspFlightWrite, adr))
    { s_join_why = 7; apu_dspJoin(apu); }  /* the batch's echo bytes come first */
#endif
  if (!apu->dspPending) {  /* marks kept for a flight; nothing owed */
    apu->ram[adr] = val;
    return;
  }
  if (apu->dspLogN == APU_DSP_LOG_LEN) {
    apu_dspSyncFor(apu, APU_DSP_SYNC_LOG_FULL);  /* clears the marks */
    apu->ram[adr] = val;
    return;
  }
  ApuDspLogEntry *e = &apu->dspLog[apu->dspLogN++];
  e->adr = adr; e->val = val; e->old = apu->ram[adr];
  e->tag = (uint16_t)apu->dspPending;
  e->isReg = APU_DSP_PAGE_BIT(apu->spc->dspWatchLog, adr) ? 0 : 2;
  apu->ram[adr] = val;
  const uint16_t rel = (uint16_t)(adr - apu->dsp->dirPage);
  if (rel < 0x400u) apu_dspMarkDirEntry(apu, (uint8_t)(rel >> 2));
  const uint16_t rel_reg = (uint16_t)(adr - (apu->dspRegs[0x5d] << 8));
  if (rel_reg < 0x400u && rel_reg != rel) apu_dspMarkDirEntry(apu, (uint8_t)(rel_reg >> 2));
}

uint32_t g_apu_stream_slow_w, g_apu_stream_slow_r;   /* statistics: SPC accesses off the fast path */
uint16_t g_apu_stream_slow_adr;                       /* the last one's address */
void apu_dspWriteSlow(Apu* apu, uint16_t adr, uint8_t val) {
#if SNESRECOMP_DSP_STREAM
  g_apu_stream_slow_w++; g_apu_stream_slow_adr = adr;
  /* Stored writes go in the ledger while a voice is untracked; a diverted
   * write leaves ARAM as it is (the S-DSP applies it at its sample). */
  if (!apu_echoDivert(apu, adr, val)) {
    if (apu->fe.untracked) apu_streamLedger(apu, adr);
    apu->ram[adr] = val;
  }
  return;
#endif
#ifdef SNESRECOMP_DSP_LAUNCH_HOOK
  apu->spc->dspViewFresh[adr >> 11] &= (uint8_t)~(1u << ((adr >> 8) & 7));
  if (!APU_DSP_PAGE_BIT(apu->spc->dspWatchLog, adr)) { apu->ram[adr] = val; return; }
#endif
  apu_dspLogRam(apu, adr, val);
}

void apu_dspViewStale(Apu* apu) {
  memset(apu->spc->dspViewFresh, 0, sizeof apu->spc->dspViewFresh);
}

static inline void apu_dspEdge(Apu* apu) {
  APU_FE_EDGE(apu);
  if (!apu->dspPending) apu_dspStartBatch(apu);
  if (++apu->dspPending >= APU_DSP_MAX_PENDING) apu_dspSyncFor(apu, APU_DSP_SYNC_FULL_BATCH);
#if SNESRECOMP_DSP_STREAM
  /* Batches end at the output ring's end (the S-DSP writes them whole). */
  else if (!(apu->dspEdges & (APU_STREAM_RING - 1))) apu_dspSyncFor(apu, APU_DSP_SYNC_FULL_BATCH);
#endif
}
#else
void apu_dspSync(Apu* apu) { (void)apu; }
void apu_dspFrameSync(Apu* apu) { (void)apu; }
void apu_dspViewStale(Apu* apu) { (void)apu; }
#endif

#if SNESRECOMP_APU_NATIVE
static inline void apu_native_timers_sync(Apu *apu);
#endif

void apu_saveload(Apu *apu, SaveLoadInfo *sli) {
  apu_dspSync(apu);  /* a save must see the DSP and ARAM up to date */
#if SNESRECOMP_APU_NATIVE
  apu_native_timers_sync(apu);  /* a save must see current timers */
#endif
  sli->func(sli, apu->ram, offsetof(Apu, pad) + 6 - offsetof(Apu, ram));
  apu_dspViewStale(apu);  /* a load replaced ARAM */
#if SNESRECOMP_SPC_DBT
  spc_dbt_flush();
#endif
  apu->timerSyncCycle = apu->cycles;  /* after a load, timers match cycles */
  dsp_saveload(apu->dsp, sli);
#if SNESRECOMP_APU_DEFER_DSP
  memcpy(apu->dspRegs, apu->dsp->ram, sizeof(apu->dspRegs));  /* after a load */
#endif
  spc_saveload(apu->spc, sli);
#if SNESRECOMP_DSP_FRONTEND
  apu->feValid = false;   /* rebuilt from the Dsp at its next use */
#if SNESRECOMP_DSP_STREAM
  apu->dspAt = apu->dspEdges;
  apu_feNow(apu);         /* (the Dsp is current: synced above, or loaded) */
#endif
#endif
}

#if SNESRECOMP_SPC_DIAGNOSTICS || SNESRECOMP_TRACE
extern uint64_t g_spc_pc_histogram[0x10000];
#else
extern uint64_t g_spc_pc_histogram[1];
#endif
extern int g_spc_pc_max_seen;

static inline void apu_cycle_reference(Apu* apu) {
#if SNESRECOMP_APU_DEFER_DSP
  apu_dspSync(apu);
#endif
  apu_drainPortQueue(apu);
  if(apu->cpuCyclesLeft == 0) {
    /* Sample PC right BEFORE running the opcode — so PC reflects the
     * instruction we're about to execute, not the post-opcode PC. */
#if SNESRECOMP_SPC_DIAGNOSTICS
    g_spc_pc_histogram[apu->spc->pc]++;
    if (apu->spc->pc > g_spc_pc_max_seen) g_spc_pc_max_seen = apu->spc->pc;
#endif
    apu->cpuCyclesLeft = spc_runOpcode(apu->spc);
  }
  apu->cpuCyclesLeft--;

  if((apu->cycles & 0x1f) == 0) {
    // every 32 cycles
    dsp_cycle(apu->dsp);
    APU_FE_EDGE(apu);
  }

  // handle timers
  extern uint64_t g_apu_timer0_total_ticks;
  for(int i = 0; i < 3; i++) {
    if(apu->timer[i].cycles == 0) {
      apu->timer[i].cycles = i == 2 ? 16 : 128;
      if(apu->timer[i].enabled) {
        apu->timer[i].divider++;
        if(apu->timer[i].divider == apu->timer[i].target) {
          apu->timer[i].divider = 0;
          apu->timer[i].counter++;
          apu->timer[i].counter &= 0xf;
          if (i == 0) g_apu_timer0_total_ticks++;
        }
      }
    }
    apu->timer[i].cycles--;
  }

  apu->cycles++;
  apu->portClock++;
  apu->timerSyncCycle = apu->cycles;  /* timers are current (eager) */
}

#if SNESRECOMP_APU_NATIVE
#include "apu_native.inc"
#endif

void apu_cycle(Apu* apu) {
#if SNESRECOMP_APU_NATIVE
  apu_runCycles_native(apu, 1);
#else
  apu_cycle_reference(apu);
#endif
}

void apu_runCycles(Apu* apu, uint32_t clocks) {
  const uint32_t tcat_prev = tcat_enter(TCAT_APU);
#ifdef N64_PHASE0
  g_apu_catchups++;
  if (g_apu_port_burst > g_apu_port_burst_max) g_apu_port_burst_max = g_apu_port_burst;
  g_apu_port_burst = 0;
#endif
#ifdef SNESRECOMP_APU_TIME
  /* Measurement builds: host time in SPC catch-ups (snesrecomp_apu_ticks,
   * the host's cycle counter), independent of the layout of other code. */
  extern uint32_t snesrecomp_apu_ticks(void);
  extern uint64_t g_apu_time_ticks;
  const uint32_t t0 = snesrecomp_apu_ticks();
#endif
#if SNESRECOMP_APU_NATIVE
  apu_runCycles_native(apu, clocks);
#else
  while (clocks--) apu_cycle(apu);
#endif
#ifdef SNESRECOMP_APU_TIME
  g_apu_time_ticks += (uint32_t)(snesrecomp_apu_ticks() - t0);
#endif
  tcat_leave(tcat_prev);
}

const char *apu_time_implementation(void) {
  return SNESRECOMP_APU_NATIVE ? "NATIVE" : "LLE";
}

#if SNESRECOMP_DSP_FRONTEND
/* The S-DSP front end inline with the SPC (docs/n64/RED_DESIGN.md). It is
 * (re)built from the Dsp, which must then be current: a deferred DSP is
 * synced first. Blocks are counted until a back end consumes them. */
uint32_t g_dsp_fe_blocks, g_dsp_fe_echo_conflicts;
ApuDspRecords g_apu_dsp_records;
enum { kFeMaxBlocks = 2048, kFeMaxKeyons = 1024 };
typedef struct { uint16_t start; uint8_t b[9]; uint8_t pad[5]; } ApuFeBlockRec;
/* Two of each: the RSP reads one batch's records while the SPC runs on. */
static ApuFeBlockRec s_feBlk[2][kFeMaxBlocks + 16] __attribute__((aligned(16)));
static uint16_t s_feKon[2][kFeMaxKeyons + 32] __attribute__((aligned(16)));
static uint32_t s_feNblk, s_feNkon;
static int s_feBuf;
static bool s_feRecOk;                 /* this batch's records stand for ARAM */
#if SNESRECOMP_ECHO_LOG
static uint32_t s_elOwnedR, s_elOwnedW, s_elClearW, s_elInRingR, s_elFeR, s_elRegs;
#endif

static inline uint8_t apu_feReg(const Apu* apu, uint8_t r) {
#if SNESRECOMP_APU_DEFER_DSP
  return apu->dspRegs[r];   /* the SPC's latest write: the front end's time */
#else
  return apu->dsp->ram[r];
#endif
}
/* A new batch's record state, from the registers at its start. */
static void apu_feBatchStart(Apu* apu) {
  s_feRecOk = apu->feValid;
}
static void apu_feBlock(void *ctx, int voice, uint32_t sample, uint16_t start, const uint8_t b[9]) {
  (void)voice; (void)sample;
#if SNESRECOMP_DSP_STREAM
  if ((((Apu *)ctx)->fe.untracked >> voice) & 1) return;   /* untracked from this read on */
#else
  (void)ctx;
#endif
  g_dsp_fe_blocks++;
  if (s_feNblk == kFeMaxBlocks) { s_feRecOk = false; return; }
  ApuFeBlockRec *r = &s_feBlk[s_feBuf][s_feNblk++];
  r->start = start;
  memcpy(r->b, b, 9);
  memset(r->pad, 0, sizeof r->pad);
}
static void apu_feKeyOn(void *ctx, int voice, uint32_t sample, uint16_t start) {
  (void)voice; (void)sample;
#if SNESRECOMP_DSP_STREAM
  if ((((Apu *)ctx)->fe.untracked >> voice) & 1) return;
#else
  (void)ctx;
#endif
  if (s_feNkon == kFeMaxKeyons) { s_feRecOk = false; return; }
  s_feKon[s_feBuf][s_feNkon++] = start;
}
#if SNESRECOMP_ECHO_IN_PLACE
/* ---- Echo in place (docs/n64/RED_DESIGN.md, "Echo in place") -----------
 * The S-DSP reads and writes its echo slots in this ARAM, at its own time,
 * behind the SPC. The front end reports each run of slots it touches
 * (apu_feEcho), stamping their lines. An SPC write to a line the S-DSP
 * still has to touch (in the window) is diverted: logged with isReg 3 and
 * not stored, for the S-DSP to apply at its sample. An SPC read of a line
 * the S-DSP still has to write waits for the S-DSP and reads past the
 * cache. Everything else is plain ARAM. */
static inline bool apu_echoLate(const Apu* apu, uint32_t stamp) {
  return (int32_t)(stamp - apu->dspAt) > 0;
}
static void apu_feEcho(void *ctx, uint32_t unit, uint32_t count, uint32_t s_end, bool writes) {
  Apu *apu = ctx;
  if (count > 0x4000) count = 0x4000;
  /* Lines [first, last] (wrapping at 4096). */
  const uint32_t first = unit >> 2, nlines = ((unit + count - 1) >> 2) - first + 1;
  const uint32_t n = nlines > 0x1000 ? 0x1000 : nlines;
  for (uint32_t k = 0; k < n; k++) {
    const uint32_t l = (first + k) & 0xfff;
    apu->echoTouch[l] = s_end;
    if (writes && (int32_t)(s_end - apu->echoDirty[l]) > 0) apu->echoDirty[l] = s_end;
#if SNESRECOMP_DSP_STREAM
    apu->echoPageTouch[l >> 4] = s_end;
    if (writes && (int32_t)(s_end - apu->echoPageDirty[l >> 4]) > 0) apu->echoPageDirty[l >> 4] = s_end;
    apu->echoWriteSlow[l >> 7] |= (uint8_t)(1u << ((l >> 4) & 7));
    if (writes) apu->echoReadSlow[l >> 7] |= (uint8_t)(1u << ((l >> 4) & 7));
#endif
  }
  /* The pending batch's runs (merged with the last when they continue it). */
  const uint32_t r = apu->echoRuns;
  if (r && r <= 16) {
    const uint32_t end = (uint32_t)apu->echoRunLine[r - 1] + apu->echoRunCount[r - 1];
    if ((end & 0xfff) == (first & 0xfff) && apu->echoRunCount[r - 1] + n <= 0x1000) {
      apu->echoRunCount[r - 1] = (uint16_t)(apu->echoRunCount[r - 1] + n);
      return;
    }
    /* Already covered (the same lines again, e.g. EDL 0). */
    const uint32_t rel = (first - apu->echoRunLine[r - 1]) & 0xfff;
    if (rel + n <= apu->echoRunCount[r - 1]) return;
  }
  if (r < 16) { apu->echoRunLine[r] = (uint16_t)(first & 0xfff); apu->echoRunCount[r] = (uint16_t)n; }
  if (r <= 16) apu->echoRuns = r + 1;
}
/* The front end, and its echo position, at the SPC's time. */
static inline void apu_echoNow(Apu* apu) {
  dsp_fe_advance(&apu->fe, apu->dspEdges);
  dsp_fe_echo_sync(&apu->fe);
#if SNESRECOMP_DSP_STREAM
  if (apu_yellow_running()) apu->dspAt = apu_yellow_progress();
#endif
}
static void apu_dspMark(Apu* apu, uint16_t start, uint32_t len, uint8_t bits);
static void apu_dspSyncFor(Apu* apu, unsigned reason);
static inline void apu_dspJoin(Apu* apu);
/* An SPC write (on a page the batch marks): diverted when its line is in
 * the window. Returns true when it was. */
static bool apu_echoDivert(Apu* apu, uint16_t adr, uint8_t val) {
  if (!apu->feValid) return false;
  apu_echoNow(apu);
  const uint32_t l = adr >> 4;
  if (!apu_echoLate(apu, apu->echoTouch[l]) && !apu_echoLate(apu, apu->echoDirty[l])) return false;
#if SNESRECOMP_DSP_STREAM
  if (apu->dspLogN >= APU_STREAM_LOG_MAX) {
    apu_dspSyncFor(apu, APU_DSP_SYNC_LOG_FULL);   /* post; the log is empty again */
#else
  if (apu->dspLogN == APU_DSP_LOG_LEN) {
    /* No room: let the S-DSP catch up, then the line is plain ARAM. */
    apu_dspSyncFor(apu, APU_DSP_SYNC_LOG_FULL);
    apu_dspJoin(apu);
    return false;
#endif
  }
  ApuDspLogEntry *e = &apu->dspLog[apu->dspLogN++];
  e->adr = adr; e->val = val; e->old = 0;
  e->tag = (uint16_t)apu->dspPending; e->isReg = 3;
  /* Applied before the sample after the SPC's (+1: a write made while no
   * batch is pending goes with the next one). */
  const uint32_t due = apu->dspEdges + 1;
  if ((int32_t)(due - apu->echoDirty[l]) > 0) apu->echoDirty[l] = due;
#if SNESRECOMP_DSP_STREAM
  if ((int32_t)(due - apu->echoPageDirty[l >> 4]) > 0) apu->echoPageDirty[l >> 4] = due;
  apu->echoWriteSlow[l >> 7] |= (uint8_t)(1u << ((l >> 4) & 7));
  apu->echoReadSlow[l >> 7] |= (uint8_t)(1u << ((l >> 4) & 7));
#else
  apu_dspMark(apu, adr, 1, APU_DSP_WATCH_LOG | APU_DSP_WATCH_SYNC);
#endif
  /* This line may hold the SPC's earlier writes: to ARAM before the S-DSP
   * reads it, and out of the cache (the S-DSP writes it). */
  apu_echo_cache_drop(apu->ram + (adr & ~15u), 16);
  return true;
}
/* An SPC read on a page marked for sync. */
uint8_t apu_dspReadSlow(Apu* apu, uint16_t adr) {
#if SNESRECOMP_DSP_STREAM
  g_apu_stream_slow_r++; g_apu_stream_slow_adr = adr;
  apu_echoNow(apu);
  if (!apu_echoLate(apu, apu->echoDirty[adr >> 4])) return apu->ram[adr];
  /* Wait for the S-DSP to reach the SPC's time: post what is owed, then
   * its progress. */
  apu_dspSyncFor(apu, APU_DSP_SYNC_ECHO_READ);
  apu_yellow_wait(apu->dspEdges);
  apu->dspAt = apu_yellow_progress();
  return apu_echo_read_uncached(apu->ram + adr);
#else
  if (!apu->feValid) { apu_dspEchoRead(apu); return apu->ram[adr]; }
  apu_echoNow(apu);
  if (!apu_echoLate(apu, apu->echoDirty[adr >> 4])) return apu->ram[adr];
  apu_dspEchoRead(apu);   /* the S-DSP to the SPC's time */
  apu_dspJoin(apu);
  return apu_echo_read_uncached(apu->ram + adr);
#endif
}
#endif

#if SNESRECOMP_DSP_STREAM
/* ---- The audio split: Blue's side (docs/n64/RED_DESIGN.md) --------------
 * The S-DSP ("Yellow", apu_yellow_* back end) owns its state while running.
 * Blue posts batches and never joins them. It waits for Yellow's progress
 * only when the SPC reads OUTX, ENVX/ENDX of a voice the front end does not
 * track (pitch modulation), or an echo line Yellow has still to write. */
uint32_t g_apu_stream_posts, g_apu_stream_waits, g_apu_stream_unsupported;

/* The pages of the echo ring in use: the S-DSP touches them every lap
 * (writes or not), whatever the stamps say yet. The lap in progress keeps
 * its length (a new EDL applies at the wrap): both count. */
static void apu_streamRegion(Apu* apu) {
  memset(apu->echoRegion, 0, sizeof apu->echoRegion);
  if (!apu->feValid) {
    memset(apu->echoRegion, 0xff, sizeof apu->echoRegion);
    memset(apu->echoWriteSlow, 0xff, sizeof apu->echoWriteSlow);
    memset(apu->echoReadSlow, 0xff, sizeof apu->echoReadSlow);
    return;
  }
  DspFrontEnd *fe = &apu->fe;
  dsp_fe_echo_sync(fe);
  uint32_t slots = fe->delay;
  const uint32_t lap = (uint32_t)fe->echo_index + fe->echo_remain;
  if (lap > slots) slots = lap;
  const uint32_t first = fe->esa >> 8, last = ((uint32_t)fe->esa + slots * 4u - 1u) >> 8;
  for (uint32_t p = first; p <= last && p < first + 256; p++) {
    apu->echoRegion[(p >> 3) & 31] |= (uint8_t)(1u << (p & 7));
    apu->echoWriteSlow[(p >> 3) & 31] |= (uint8_t)(1u << (p & 7));
    apu->echoReadSlow[(p >> 3) & 31] |= (uint8_t)(1u << (p & 7));
  }
}
/* The exact tests behind the page bits; a bit that no longer stands for
 * anything is cleared (stamps only fall behind the S-DSP's progress, and
 * every site that sets a stamp or the region sets the bits). */
uint32_t g_apu_stream_slow_bad;
bool apu_streamWriteSlowNow(Apu* apu, uint32_t p) {
  if (apu_streamWriteExact(apu, p)) return true;
  apu->echoWriteSlow[p >> 3] &= (uint8_t)~(1u << (p & 7));
  return false;
}
bool apu_streamReadSlowNow(Apu* apu, uint32_t p) {
  if (apu_streamReadExact(apu, p)) return true;
  if (!((apu->echoRegion[p >> 3] >> (p & 7)) & 1)) apu->echoReadSlow[p >> 3] &= (uint8_t)~(1u << (p & 7));
  return false;
}

static void apu_streamEnsure(Apu* apu) {
  if (apu_yellow_running()) return;
  /* The Dsp is current at dspAt, the untracked voices as they were then
   * (later changes are in the log). */
  apu_yellow_start(apu->dsp, apu->ram, apu->dspAt, apu->dspUntracked, apu->dspConflict);
  /* The consumer reads the back end's ring from now on, in absolute
   * samples (restarting where it left off when the counts still agree). */
  Dsp *d = apu->dsp;
  if (!d->outRing || d->sampleWrite != apu->dspAt) d->sampleRead = d->sampleWrite = apu->dspAt;
  d->outRing = apu_yellow_ring(&d->outMask);
}

/* The consumer (Green): the samples the S-DSP has finished become readable. */
void apu_dspOutput(Apu* apu) {
  if (apu_yellow_running()) apu->dsp->sampleWrite = apu_yellow_publish();
}

ApuLedgerEntry g_apu_ledger[APU_LEDGER_LEN] __attribute__((aligned(16)));
uint32_t g_apu_ledger_head, g_apu_ledger_tail;
uint32_t g_apu_ledger_tail_pub[4] __attribute__((aligned(16)));   /* (a cache line of its own) */
uint32_t g_apu_ledger_page[256] __attribute__((aligned(16)));
uint32_t g_apu_stream_ledger_waits;
/* Entries the S-DSP has passed go. */
static void apu_streamLedgerDrop(Apu* apu) {
  /* (Read through the uncached alias, as the entries are written.) */
  while (g_apu_ledger_head != g_apu_ledger_tail &&
         (int32_t)(((volatile ApuLedgerEntry *)apu_host_uncached(
                       &g_apu_ledger[g_apu_ledger_head & (APU_LEDGER_LEN - 1)]))->time - apu->dspAt) <= 0)
    g_apu_ledger_head++;
}
/* An SPC write to ARAM while a voice is untracked: its old value, for the
 * S-DSP's reads at earlier samples. */
void apu_streamLedger(Apu* apu, uint16_t adr) {
  /* (At most LEN - 1 entries: the slot before the oldest keeps an expired
   * entry, which ends the microcode's scan back from the tail.) */
  if (g_apu_ledger_tail - g_apu_ledger_head >= APU_LEDGER_LEN - 1) {
    apu->dspAt = apu_yellow_running() ? apu_yellow_progress() : apu->dspEdges;
    apu_streamLedgerDrop(apu);
    if (g_apu_ledger_tail - g_apu_ledger_head >= APU_LEDGER_LEN - 1) {
      /* Full: let the S-DSP reach the SPC's time; every entry expires. */
      g_apu_stream_ledger_waits++;
      apu_dspSyncFor(apu, APU_DSP_SYNC_LOG_FULL);
      apu_yellow_wait(apu->dspEdges);
      apu->dspAt = apu_yellow_progress();
      apu_streamLedgerDrop(apu);
    }
  }
  /* Through the uncached alias, in this order: the entry, the tail, the
   * page's time; the data itself reaches RDRAM later (or never sooner). */
  ApuLedgerEntry *e = apu_host_uncached(&g_apu_ledger[g_apu_ledger_tail & (APU_LEDGER_LEN - 1)]);
  e->time = apu->dspEdges;
  e->adr = adr;
  e->old = apu->ram[adr];
  e->pad = 0;
  g_apu_ledger_tail++;
  *(volatile uint32_t *)apu_host_uncached(&g_apu_ledger_tail_pub[0]) = g_apu_ledger_tail;
  ((volatile uint32_t *)apu_host_uncached(g_apu_ledger_page))[adr >> 8] = apu->dspEdges;
}

static void apu_streamPost(Apu* apu) {
  apu_streamEnsure(apu);
  apu_feTakeRecords(apu);
  if (apu->dspPending && !g_apu_dsp_records.valid) g_apu_stream_unsupported++;
  apu_yellow_post(apu->dspLog, apu->dspLogN, apu->dspPending);
  apu->dspPending = 0;
  apu->dspLogN = 0;
  g_apu_stream_posts++;
  apu->dspAt = apu_yellow_progress();
  apu_streamLedgerDrop(apu);
}

/* Hand the S-DSP's state back to the Dsp (current at dspEdges after it). */
static void apu_streamStop(Apu* apu) {
  if (apu->dspPending || apu->dspLogN) apu_streamPost(apu);   /* (starts it if need be) */
  if (!apu_yellow_running()) return;   /* the Dsp is current already */
  apu_yellow_stop(apu->dsp);
  apu->dspAt = apu->dspEdges;
  apu->dspUntracked = apu->fe.untracked;
    apu->dspConflict = apu->fe.conflict;   /* (the front end is at dspEdges: posted above) */
  apu->dsp->sampleWrite = apu_yellow_publish();   /* every sample, readable (= dspAt) */
  /* ENVX/OUTX the SPC wrote since the last sample read back as written. */
  for (int r = 8; r < 0x80; r += 16)
    for (int k = 0; k < 2; k++)
      if (apu->dspRegAt[r + k] == apu->dspEdges + 1) apu->dsp->ram[r + k] = apu->dspRegs[r + k];
}

/* The S-DSP's registers as of the SPC's time: post and wait. */
static const ApuYellowRegs *apu_streamRegs(Apu* apu) {
  apu_dspSyncFor(apu, APU_DSP_SYNC_REG_READ);
  apu_streamEnsure(apu);
  apu_yellow_wait(apu->dspEdges);
  g_apu_stream_waits++;
  apu->dspAt = apu_yellow_progress();
  return apu_yellow_regs();
}

/* An SPC read of ENVX, OUTX or ENDX. */
static uint8_t apu_streamRegRead(Apu* apu, uint8_t r) {
  if (apu->dspRegAt[r] == apu->dspEdges + 1) return apu->dspRegs[r];   /* written this sample */
  DspFrontEnd *fe = &apu->fe;
  const int v = r >> 4;
  if (r == 0x7c) {
    uint8_t val = dsp_fe_endx(fe) & (uint8_t)~fe->untracked;
    if (fe->untracked) val |= apu_streamRegs(apu)->endx & fe->untracked;
    return val;
  }
  if ((r & 0xf) == 8 && dsp_fe_tracked(fe, v)) return dsp_fe_envx(fe, v);
  const ApuYellowRegs *y = apu_streamRegs(apu);
  return (r & 0xf) == 8 ? y->envx[v] : y->outx[v];
}
#endif

/* A read of bytes the S-DSP's echo has written since the front end began
 * (exactly: the front end tracks the echo position) reads the S-DSP's own
 * output, which ARAM here may not hold yet: such a batch runs in C. Called
 * mid-run with echo_owned current to the sample being read. Echo in place:
 * only bytes the S-DSP has still to write (in the window) conflict; the
 * read may have pulled their line into the cache, so it is dropped. */
static void apu_feRead(void *ctx, int voice, uint32_t sample, uint16_t adr, unsigned len) {
  Apu *apu = ctx;
  (void)voice; (void)sample;
#if SNESRECOMP_ECHO_IN_PLACE
  bool late = false;
  for (uint32_t l = adr >> 4; l <= ((uint32_t)adr + len - 1) >> 4; l++)
    if (apu_echoLate(apu, apu->echoDirty[l & 0xfff])) {
      late = true;
      apu_echo_cache_drop(apu->ram + ((l & 0xfff) << 4), 16);
    }
  if (!late) return;
#else
  if (!dsp_fe_echo_owned(&apu->fe, adr, len)) return;
#endif
#if SNESRECOMP_ECHO_LOG
  s_elFeR++;
#endif
  g_dsp_fe_echo_conflicts++;
#if SNESRECOMP_DSP_STREAM
  /* The bytes are the S-DSP's to write yet: the voice is the S-DSP's to
   * read from here (an untracked voice, as with pitch modulation). */
  dsp_fe_untrack(&apu->fe, voice);
  ApuDspLogEntry *e = &apu->dspLog[apu->dspLogN++];
  e->adr = (uint16_t)(1u << voice); e->val = 0; e->old = 0;
  e->tag = (uint16_t)(sample - (apu->dspEdges - apu->dspPending)); e->isReg = 4;
#else
  s_feRecOk = false;
#endif
}
/* At a batch's launch: hand its records over and start the next set. */
static void apu_feTakeRecords(Apu* apu) {
  if (apu->feValid) dsp_fe_advance(&apu->fe, apu->dspEdges);
#if SNESRECOMP_ECHO_IN_PLACE
  if (apu->feValid) dsp_fe_echo_sync(&apu->fe);   /* the batch's echo lines, all of them */
  g_apu_dsp_records.nruns = apu->feValid ? apu->echoRuns : 17;
  memcpy(g_apu_dsp_records.run_line, apu->echoRunLine, sizeof apu->echoRunLine);
  memcpy(g_apu_dsp_records.run_count, apu->echoRunCount, sizeof apu->echoRunCount);
  apu->echoRuns = 0;
#endif
  g_apu_dsp_records.blocks = s_feBlk[s_feBuf];
  g_apu_dsp_records.nblocks = s_feNblk;
  g_apu_dsp_records.keyons = s_feKon[s_feBuf];
  g_apu_dsp_records.nkeyons = s_feNkon;
#if SNESRECOMP_DSP_STREAM
  g_apu_dsp_records.valid = s_feRecOk && apu->feValid;   /* untracked voices: the ledger */
#else
  g_apu_dsp_records.valid = s_feRecOk && apu->feValid && !apu->fe.untracked;
#endif
  s_feBuf ^= 1;
  s_feNblk = s_feNkon = 0;
  s_feRecOk = false;   /* until the next batch starts */
}
#if SNESRECOMP_ECHO_LOG
/* Host diagnostic: which echo paths a game hits. Counts SPC reads and
 * writes of bytes the echo has written (owned), writes into the ring
 * region while echo writes are off (a clear), front-end reads of owned
 * bytes, and lists every ESA/EDL/FLG-bit-5 change. */
static Apu *s_elApu;
static void apu_feEchoLogEnd(void) {
  if (s_elApu && s_elApu->feValid) {
    DspFrontEnd *fe = &s_elApu->fe;
    dsp_fe_echo_sync(fe);
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof fe->echo_owned / 4; i++) n += __builtin_popcount(fe->echo_owned[i]);
    fprintf(stderr, "[echolog] end: sample %u, ESA %04x, ring %u bytes, echo writes %s, owned %u bytes; "
            "Dsp FLG %02x echoWrites %d\n", fe->now, fe->esa, fe->delay * 4u, fe->echo_writes ? "on" : "off",
            n * 4, s_elApu->dsp->ram[0x6c], s_elApu->dsp->echoWrites);
  }
  fprintf(stderr, "[echolog] SPC reads of owned bytes %u, writes to owned bytes %u; "
          "writes into the region with echo writes off %u, reads there %u; "
          "front-end reads of owned bytes %u; ESA/EDL/FLG5 changes %u\n",
          s_elOwnedR, s_elOwnedW, s_elClearW, s_elInRingR, s_elFeR, s_elRegs);
}
static bool apu_feInRegion(const DspFrontEnd *fe, uint16_t adr) {
  return (uint16_t)(adr - fe->esa) < (uint32_t)fe->delay * 4u;
}
void apu_feEchoLog(Apu* apu, uint16_t adr, int isWrite) {
  static bool reg;
  if (!reg) { reg = true; atexit(apu_feEchoLogEnd); }
  s_elApu = apu;
  if (!apu->feValid) return;
  DspFrontEnd *fe = &apu->fe;
  dsp_fe_advance(fe, apu->dspEdges);
  dsp_fe_echo_sync(fe);
  const bool owned = dsp_fe_echo_owned(fe, adr, 1);
  uint32_t *c = owned ? (isWrite ? &s_elOwnedW : &s_elOwnedR)
                      : (!fe->echo_writes && apu_feInRegion(fe, adr)) ? (isWrite ? &s_elClearW : &s_elInRingR) : NULL;
  if (!c) return;
  if (owned && *c < 4)
    fprintf(stderr, "[echolog] sample %u: SPC %s %04x (owned; ESA %04x len %u, writes %s) pc %04x\n",
            fe->now, isWrite ? "write" : "read", adr, fe->esa, fe->delay * 4u,
            fe->echo_writes ? "on" : "off", apu->spc->pc);
  (*c)++;
}
static void apu_feEchoLogReg(Apu* apu, uint8_t r, uint8_t v) {
  const uint8_t old = apu_feReg(apu, r);
  if (!(r == 0x6d || r == 0x7d || (r == 0x6c && ((old ^ v) & 0x20)))) return;
  if (r != 0x6c && old == v) return;
  if (s_elRegs++ < 40)
    fprintf(stderr, "[echolog] sample %u: %s %02x -> %02x\n", apu->dspEdges,
            r == 0x6d ? "ESA" : r == 0x7d ? "EDL" : "FLG echo-writes-off bit", r == 0x6c ? (old >> 5) & 1 : old,
            r == 0x6c ? (v >> 5) & 1 : v);
}
#endif
static void apu_feNow(Apu* apu) {
  /* Voices left untracked by pitch modulation: rebuild once it is off. */
  const bool rebuild = apu->feValid && apu->fe.untracked && !(apu_feReg(apu, 0x2d) & 0xfe);
  if (!apu->feValid || rebuild) {
#if SNESRECOMP_APU_DEFER_DSP
    apu_dspSync(apu);   /* (with the old front end still valid: its batch completes) */
#endif
    dsp_fe_init(&apu->fe, apu->dsp, apu->ram, apu->dspEdges, apu_feBlock, apu);
    apu->fe.on_keyon = apu_feKeyOn;
    apu->fe.on_read = apu_feRead;
#if SNESRECOMP_ECHO_IN_PLACE
    apu->fe.on_echo = apu_feEcho;
    apu->dspAt = apu->dspEdges;    /* synced above */
#endif
#if SNESRECOMP_DSP_STREAM
    apu_streamRegion(apu);
    apu->dspUntracked = apu->fe.untracked;
    apu->dspConflict = apu->fe.conflict;
#endif
    apu->feValid = true;
  }
  dsp_fe_advance(&apu->fe, apu->dspEdges);
}
#if SNESRECOMP_DSP_FRONTEND_CHECK
/* Check builds (the DSP in lockstep, not deferred, so the Dsp is current at
 * every SPC access): the front end must agree with the Dsp on every tracked
 * voice's position, latch and envelope, and on ENDX. */
uint32_t g_dsp_fe_checks, g_dsp_fe_bad;
static void apu_feCheck(Apu* apu) {
  DspFrontEnd *fe = &apu->fe;
  const Dsp *d = apu->dsp;
  if ((++g_dsp_fe_checks & 0x3fff) == 0)
    fprintf(stderr, "[dspfe] %u checks, %u differ; %u blocks; work on %u samples of %u; untracked %02x\n",
            g_dsp_fe_checks, g_dsp_fe_bad, g_dsp_fe_blocks, fe->events, apu->dspEdges, fe->untracked);
  int bad = (fe->endx ^ d->ram[0x7c]) & (uint8_t)~fe->untracked;
  for (int c = 0; c < 8 && !bad; c++) {
    if (!dsp_fe_tracked(fe, c)) continue;
    const DspChannel *ch = &d->channel[c];
    const DspFeVoice *v = &fe->v[c];
    const uint8_t envx = dsp_fe_envx(fe, c);
    bad = dsp_fe_counter(fe, c) != ch->pitchCounter || v->offset != ch->decodeOffset ||
          v->prev_flags != ch->previousFlags || v->key_on != ch->keyOn ||
          v->gain != ch->gain || v->state != ch->adsrState || v->rate_counter != ch->rateCounter ||
          envx != d->ram[c * 16 + 8];
    if (bad && g_dsp_fe_bad < 10)
      fprintf(stderr, "[dspfe] sample %u voice %d: counter %04x/%04x offset %04x/%04x gain %03x/%03x "
              "state %u/%u\n", fe->now, c, dsp_fe_counter(fe, c), ch->pitchCounter, v->offset,
              ch->decodeOffset, v->gain, ch->gain, v->state, ch->adsrState);
  }
  if (bad) {
    if (g_dsp_fe_bad < 10 && ((fe->endx ^ d->ram[0x7c]) & (uint8_t)~fe->untracked))
      fprintf(stderr, "[dspfe] sample %u: ENDX %02x/%02x\n", fe->now, fe->endx, d->ram[0x7c]);
    g_dsp_fe_bad++;
  }
}
#else
#define apu_feCheck(apu) ((void)0)
#endif
#endif

uint8_t apu_cpuRead(Apu* apu, uint16_t adr) {
  switch(adr) {
    case 0xf0:
    case 0xf1:
    case 0xfa:
    case 0xfb:
    case 0xfc: {
      return 0;
    }
    case 0xf2: {
      return apu->dspAdr;
    }
    case 0xf3: {
#if SNESRECOMP_DSP_FRONTEND
      apu_feNow(apu);
      apu_feCheck(apu);
#endif
#if SNESRECOMP_DSP_STREAM
      {
        const uint8_t r = apu->dspAdr & 0x7f;
        if ((r & 0xf) == 8 || (r & 0xf) == 9 || r == 0x7c) return apu_streamRegRead(apu, r);
        return apu->dspRegs[r];
      }
#endif
#if SNESRECOMP_APU_DEFER_DSP
      {
        const uint8_t r = apu->dspAdr & 0x7f;
        /* Only ENVX ($x8), OUTX ($x9) and ENDX ($7C) change as the DSP runs;
         * every other register reads back the SPC's latest write. */
        if ((r & 0xf) == 8 || (r & 0xf) == 9 || r == 0x7c) {
          apu_dspSyncFor(apu, APU_DSP_SYNC_REG_READ);  /* also joins a flight */
          return dsp_read(apu->dsp, r);
        }
        return apu->dspRegs[r];
      }
#endif
      return dsp_read(apu->dsp, apu->dspAdr & 0x7f);
    }
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7: {
      uint8_t v = apu->inPorts[adr - 0xf4];
      audio_trace_on_spc_port_read((uint8_t)(adr - 0xf4), v);
#if defined(SNESRECOMP_TRACE) && SNESRECOMP_TRACE
      if (getenv("SNESRECOMP_SPC_PORT_TRACE")) {
        static unsigned long n;
        if (n++ < 2000000)
          fprintf(stderr, "[spcport] pc=%04X port=%d val=%02X clock=%llu q=%u\n",
                  apu->spc->pc, adr - 0xf4, v,
                  (unsigned long long)apu->portClock,
                  (unsigned)(apu->portQTail - apu->portQHead));
      }
#endif
      return v;
    }
    case 0xf8:
    case 0xf9: {
      return apu->inPorts[adr - 0xf4];
    }
    case 0xfd:
    case 0xfe:
    case 0xff: {
#if SNESRECOMP_APU_NATIVE
      apu_native_timers_sync(apu);
#endif
      uint8_t ret = apu->timer[adr - 0xfd].counter;
      apu->timer[adr - 0xfd].counter = 0;
      return ret;
    }
  }
  if(apu->romReadable && adr >= 0xffc0) {
    return bootRom[adr - 0xffc0];
  }
  return apu_spcReadRam(apu, adr);
}

/* Diagnostic counters: track SPC writes to specific addresses so we
 * can see whether the engine is touching outPorts at all. */
#if SNESRECOMP_SPC_DIAGNOSTICS || SNESRECOMP_TRACE
uint64_t g_spc_write_counts[0x100] = {0};

/* SPC PC histogram. Sampled once per apu_cycle that starts a new
 * opcode. Lets us answer "which PCs does the SPC spend time in?" */
uint64_t g_spc_pc_histogram[0x10000] = {0};
int g_spc_pc_max_seen = 0;
/* Per-value count for outPorts $F4-$F7. Index = (port_idx * 256) + val. */
uint64_t g_spc_outport_value_counts[4 * 256] = {0};
/* Last 32 outPort writes as a ring buffer: [adr, val] pairs. */
typedef struct { uint8_t adr; uint8_t val; } SpcWriteRec;
SpcWriteRec g_spc_recent_outport_writes[32];
int g_spc_recent_outport_idx = 0;
#else
uint64_t g_spc_write_counts[1] = {0};
uint64_t g_spc_pc_histogram[1] = {0};
int g_spc_pc_max_seen = 0;
uint64_t g_spc_outport_value_counts[1] = {0};
typedef struct { uint8_t adr; uint8_t val; } SpcWriteRec;
SpcWriteRec g_spc_recent_outport_writes[1];
int g_spc_recent_outport_idx = 0;
#endif

void apu_cpuWrite(Apu* apu, uint16_t adr, uint8_t val) {
#if SNESRECOMP_SPC_DIAGNOSTICS
  if (adr < 0x100) g_spc_write_counts[adr]++;
  if (adr >= 0xF4 && adr <= 0xF7) {
    int port = adr - 0xF4;
    g_spc_outport_value_counts[port * 256 + val]++;
    int i = g_spc_recent_outport_idx++ & 31;
    g_spc_recent_outport_writes[i].adr = (uint8_t)adr;
    g_spc_recent_outport_writes[i].val = val;
  }
#endif
#if SNESRECOMP_APU_NATIVE
  if (adr == 0xf1 || (adr >= 0xfa && adr <= 0xfc)) apu_native_timers_sync(apu);
#endif
  switch(adr) {
    case 0xf0: {
      break; // test register
    }
    case 0xf1: {
      for(int i = 0; i < 3; i++) {
        if(!apu->timer[i].enabled && (val & (1 << i))) {
          apu->timer[i].divider = 0;
          apu->timer[i].counter = 0;
        }
        apu->timer[i].enabled = val & (1 << i);
      }
      if(val & 0x10) {
        apu->inPorts[0] = 0;
        apu->inPorts[1] = 0;
      }
      if(val & 0x20) {
        apu->inPorts[2] = 0;
        apu->inPorts[3] = 0;
      }
      apu->romReadable = val & 0x80;
      break;
    }
    case 0xf2: {
      apu->dspAdr = val;
      break;
    }
    case 0xf3: {
#if SNESRECOMP_DSP_FRONTEND
      if (apu->dspAdr < 0x80) {
        apu_feNow(apu);
        apu_feCheck(apu);
#if SNESRECOMP_ECHO_LOG
        apu_feEchoLogReg(apu, apu->dspAdr, val);
#endif
        dsp_fe_write(&apu->fe, apu->dspAdr, val);
#if SNESRECOMP_DSP_STREAM
        if (apu->dspAdr == 0x6c || apu->dspAdr == 0x6d || apu->dspAdr == 0x7d) apu_streamRegion(apu);
#endif
      }
#endif
#if SNESRECOMP_DSP_STREAM
      /* Every register write goes to the S-DSP in the log (none to the Dsp). */
      if (apu->dspAdr < 0x80) {
        const uint8_t r = apu->dspAdr;
        /* Pitch modulation turning on or off: a batch has it throughout or
         * not at all (the back end runs the former on its voice-major path). */
        if (r == 0x2d && !(apu->dspRegs[0x2d] & 0xfe) != !(val & 0xfe))
          apu_dspSyncFor(apu, APU_DSP_SYNC_LOG_FULL);
        apu->dspRegs[r] = r == 0x7c ? 0 : val;
        apu->dspRegAt[r] = apu->dspEdges + 1;
        if (apu->dspLogN >= APU_STREAM_LOG_MAX) apu_dspSyncFor(apu, APU_DSP_SYNC_LOG_FULL);
        ApuDspLogEntry *e = &apu->dspLog[apu->dspLogN++];
        e->adr = r; e->val = val; e->old = 0;
        e->tag = (uint16_t)apu->dspPending; e->isReg = 1;
      }
      break;
#endif
#if SNESRECOMP_APU_DEFER_DSP
      if(apu->dspAdr < 0x80)
        apu->dspRegs[apu->dspAdr] = apu->dspAdr == 0x7c ? 0 : val;
      if(!apu->dspPending) apu_dspJoin(apu);  /* direct write: Dsp must be current */
      if(apu->dspAdr < 0x80 && apu->dspPending) {
        const uint8_t r = apu->dspAdr;
        if(apu->dspLogN == APU_DSP_LOG_LEN) {
          apu_dspSyncFor(apu, APU_DSP_SYNC_LOG_FULL);
          apu_dspJoin(apu);  /* the write below goes to the Dsp directly */
        } else {
          ApuDspLogEntry *e = &apu->dspLog[apu->dspLogN++];
          e->adr = r; e->val = val; e->old = 0;
          e->tag = (uint16_t)apu->dspPending; e->isReg = 1;
          if((r & 0xf) == 4) apu_dspMarkDirEntry(apu, val);  /* new SRCN */
          /* FLG/ESA/EDL move the echo window, DIR the directory */
          if(r == 0x6c || r == 0x6d || r == 0x7d || r == 0x5d) apu_dspMarkRegWindows(apu);
          break;
        }
      }
#endif
      if(apu->dspAdr < 0x80) dsp_write(apu->dsp, apu->dspAdr, val);
      break;
    }
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7: {
      audio_trace_on_spc_port_write((uint8_t)(adr - 0xf4), val);
#ifdef N64_PHASE0
      { extern uint32_t g_apu_outport_changes; if (apu->outPorts[adr - 0xf4] != val) g_apu_outport_changes++; }
#endif
      apu->outPorts[adr - 0xf4] = val;
      break;
    }
    case 0xf8:
    case 0xf9: {
      apu->inPorts[adr - 0xf4] = val;
      break;
    }
    case 0xfa:
    case 0xfb:
    case 0xfc: {
      apu->timer[adr - 0xfa].target = val;
      break;
    }
  }
#if defined(SNESRECOMP_TRACE) && SNESRECOMP_TRACE
  if ((adr >= 0x0200 && adr < 0x02c0) || (adr >= 0x3900 && adr < 0x3c00)) {
    if (getenv("SNESRECOMP_RAMWRITE_TRACE")) {
      static unsigned long n;
      if (n++ < 4000000)
        fprintf(stderr, "[ramw] pc=%04X adr=%04X val=%02X\n", apu->spc->pc, adr, val);
    }
  }
#endif
  apu_spcWriteRam(apu, adr, val);
}
