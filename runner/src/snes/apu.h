#ifndef APU_H
#define APU_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Apu Apu;

#include "snes.h"
#include "spc.h"
#include "dsp.h"

typedef struct Timer {
  uint8_t cycles;
  uint8_t divider;
  uint8_t target;
  uint8_t counter;
  bool enabled;
} Timer;

/* CPU->APU bus events are timestamped in guest APU cycles. The audio callback
 * may execute the SPC ahead of or behind the CPU thread, but it must not turn
 * two writes from different emulated frames into back-to-back host mutations.
 * A power-of-two queue preserves global CPU bus order. If it ever fills, the
 * caller advances the SPC until space exists; no event is overwritten or
 * force-applied before its guest timestamp. */
#define APU_PORT_QUEUE_LEN 1024u

/* SNESRECOMP_APU_DEFER_DSP (native APU runner only, default 0): the S-DSP
 * runs in batches behind the SPC instead of at every sample edge. SPC writes
 * to ARAM and DSP registers during a batch are logged with the number of
 * samples then owed; apu_dspSync() unwinds the ARAM writes and replays the
 * owed samples with the log reapplied in order, so the DSP sees exactly the
 * memory and registers it would have. A sync is forced wherever the SPC or
 * the host could observe the difference: SPC reads of DSP registers or of
 * the echo window, writes to FLG/ESA/EDL, a full log or batch, save/load,
 * and the host's audio and frame boundaries. */
#ifndef SNESRECOMP_APU_DEFER_DSP
#define SNESRECOMP_APU_DEFER_DSP 0
#endif
#if SNESRECOMP_APU_DEFER_DSP && !SNESRECOMP_APU_NATIVE
#error SNESRECOMP_APU_DEFER_DSP requires SNESRECOMP_APU_NATIVE
#endif
#define APU_DSP_LOG_LEN 2048u
/* SNESRECOMP_APU_DEFER_DSP_VERIFY (debug): log every SPC write, replay each
 * batch both from the full log and the way page-watch tracking would, and
 * report any difference (g_apu_dsp_verify_mismatches). */
#ifndef SNESRECOMP_APU_DEFER_DSP_VERIFY
#define SNESRECOMP_APU_DEFER_DSP_VERIFY 0
#endif
#define APU_DSP_WATCH_LOG 1u
#define APU_DSP_WATCH_SYNC 2u
enum { APU_DSP_SYNC_HOST, APU_DSP_SYNC_REG_READ, APU_DSP_SYNC_ECHO_READ,
       APU_DSP_SYNC_ECHO_REG, APU_DSP_SYNC_LOG_FULL, APU_DSP_SYNC_FULL_BATCH,
       APU_DSP_SYNC_FRAME, APU_DSP_SYNC_REASONS };
#define APU_DSP_MAX_PENDING 256u

#include "apu_dsp_batch.h"

typedef struct ApuPortWrite {
  uint64_t target_cycle; /* apply when portClock reaches this APU cycle */
  uint8_t port;          /* 0-3 */
  uint8_t val;
} ApuPortWrite;

struct Apu {
  Spc* spc;
  Dsp* dsp;
  uint8_t ram[0x10000];
  bool romReadable;
  uint8_t dspAdr;
  uint32_t cycles;
  uint8_t inPorts[6]; // includes 2 bytes of ram
  uint8_t outPorts[4];
  Timer timer[3];
  uint8_t cpuCyclesLeft;
  uint8_t pad[6];
  /* Port event state must stay after pad: apu_saveload snapshots [ram,pad+6),
   * and that savestate layout is frozen. Callback lead is host state, so the
   * queue and its mapping are deliberately reset rather than serialized.
   * The scalars precede the 16 KB queue so they share cache lines with the
   * fields above; after it they would sit exactly 16 KB from those fields and
   * evict them on every SPC instruction in an 8 KB direct-mapped cache. */
  uint32_t portQHead;
  uint32_t portQTail;
  uint64_t portClock;        /* APU cycles executed since reset */
  uint64_t portGuestAnchor;  /* guest-cycle origin for current mapping */
  uint64_t portTargetAnchor; /* matching portClock origin */
  uint64_t portLastGuest;
  uint64_t portLastTarget;
  bool portTimeValid;
  /* Native APU builds advance the timers lazily: they are current through
   * cycle timerSyncCycle - 1. Host state, re-derived on load and reset. */
  uint32_t timerSyncCycle;
  ApuPortWrite portQueue[APU_PORT_QUEUE_LEN];
#if SNESRECOMP_APU_DEFER_DSP
  /* Deferred DSP (host state; empty after every sync, save and reset). */
  uint32_t dspPending;
  uint32_t dspLogN;
  ApuDspLogEntry dspLog[APU_DSP_LOG_LEN];
  /* The last value written to each DSP register. Registers other than
   * ENVX, OUTX and ENDX change only by writes, so reads of them are answered
   * here without catching up a deferred (or in-flight) DSP. */
  uint8_t dspRegs[0x80];
  /* A batch running elsewhere (SNESRECOMP_DSP_LAUNCH_HOOK), and its echo
   * window, [lo, lo + len) modulo 64 KB, which the SPC must not touch
   * unjoined. */
  bool dspInFlight;
  uint16_t dspFlightEchoLo;
  uint32_t dspFlightEchoLen;
#endif
};

/*
 * CPU->APU port-write scheduler state, for rollback snapshots.
 *
 * apu_saveload deliberately stops at `pad` and leaves this out: for an
 * ordinary savestate the queue is host lead that is better reset than
 * restored. Rollback cannot accept that. The queue schedules guest-visible
 * port writes against `portClock`, a monotonic APU-cycle count that a
 * snapshot load does not rewind — so after a rewind the mapping from guest
 * cycles to APU cycles no longer matches the SPC/DSP state the snapshot put
 * back. Two peers rewinding at different ticks then drift apart in APU
 * handshake timing, which is a desync with no visible cause. Rollback
 * snapshots carry this alongside the guest blob.
 */
typedef struct ApuPortSched {
  ApuPortWrite queue[APU_PORT_QUEUE_LEN];
  uint32_t qHead;
  uint32_t qTail;
  uint64_t clock;
  uint64_t guestAnchor;
  uint64_t targetAnchor;
  uint64_t lastGuest;
  uint64_t lastTarget;
  bool     timeValid;
} ApuPortSched;

void apu_port_sched_save(const Apu *apu, ApuPortSched *out);
void apu_port_sched_restore(Apu *apu, const ApuPortSched *in);

/* Bring a deferred DSP up to date (no-op without SNESRECOMP_APU_DEFER_DSP).
 * Call before host code reads DSP output or writes ARAM directly. */
void apu_dspSync(Apu* apu);
#if SNESRECOMP_APU_DEFER_DSP
void apu_dspJoinNow(Apu* apu);
#endif
/* Frame boundaries. Normally apu_dspSync. With SNESRECOMP_DSP_HOST_ASYNC
 * (a host whose frame-boundary code reads no DSP state, e.g. the N64) the
 * owed samples are started on the batch hook without waiting (or, while a
 * batch is still running, left owed for the next one); the output reaches
 * the ring when the batch is joined. Emulation is unaffected: the
 * SPC only sees DSP results through syncs and joins. */
#ifndef SNESRECOMP_DSP_HOST_ASYNC
#define SNESRECOMP_DSP_HOST_ASYNC 0
#endif
void apu_dspFrameSync(Apu* apu);
#if SNESRECOMP_APU_DEFER_DSP
void apu_dspLogRam(Apu* apu, uint16_t adr, uint8_t val);
void apu_dspEchoRead(Apu* apu);
extern uint32_t g_apu_dsp_verify_mismatches;
extern uint32_t g_apu_dsp_sync_count[APU_DSP_SYNC_REASONS];
extern uint32_t g_apu_dsp_sync_samples[APU_DSP_SYNC_REASONS];
#endif

Apu* apu_init();
void apu_free(Apu* apu);
void apu_reset(Apu* apu);
void apu_cycle(Apu* apu);
/* Advance one bounded job. The build selects scalar clocks or native event
 * spans; both preserve port order, SPC/timer state and DSP sample edges. */
void apu_runCycles(Apu* apu, uint32_t clocks);
const char *apu_time_implementation(void);
uint8_t apu_cpuRead(Apu* apu, uint16_t adr);
void apu_cpuWrite(Apu* apu, uint16_t adr, uint8_t val);
void apu_saveload(Apu *apu, SaveLoadInfo *sli);

/* Immediate visibility is reserved for boot and synchronous protocol code
 * which advances the SPC itself. Caller holds RtlApuLock. */
void apu_writePortNow(Apu* apu, uint8_t port, uint8_t val);
/* Map a monotonic guest APU-cycle timestamp onto the live SPC timeline and
 * enqueue the write. False means the caller must advance the APU and retry. */
bool apu_schedulePortWrite(Apu* apu, uint8_t port, uint8_t val,
                           uint64_t guest_cycle);
uint32_t apu_portQueueDepth(const Apu* apu);
/* Advance real SPC execution until every queued bus event has landed. */
void apu_drainDuePortWrites(Apu* apu);
bool apu_runUntilPortQueueEmpty(Apu* apu, uint32_t max_cycles);
/* Advance the live SPC to a guest timestamp using the same mapping as queued
 * CPU writes. This is the fast-forward coupling point: game frames and SPC
 * state advance together even when the host audio device cannot play them. */
bool apu_runToGuestCycle(Apu* apu, uint64_t guest_cycle,
                         uint32_t max_cycles);
/* HLE for a declared live transfer protocol: wait for the driver-ready AA/BB
 * pair, then deliver the standard CC terminator and wait for its echo. */
bool apu_waitForTransferReady(Apu* apu, uint8_t request_port,
                              uint8_t request_value, uint32_t max_cycles);
bool apu_finishHleTransfer(Apu* apu, uint16_t final_pc,
                           uint32_t max_cycles);
/* Drop pending events and reset the guest-to-SPC mapping. */
void apu_clearPortQueue(Apu* apu);

#endif
