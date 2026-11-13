#ifndef SPC_H
#define SPC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Spc Spc;

#include "apu.h"
#include "saveload.h"

#ifndef SNESRECOMP_APU_DEFER_DSP
#define SNESRECOMP_APU_DEFER_DSP 0
#endif

struct Spc {
  Apu* apu;
#if SNESRECOMP_APU_DEFER_DSP
  /* Deferred-DSP page marks, one bit per ARAM page (see apu.h). Kept here,
   * beside the registers the SPC touches on every instruction, so the check
   * on each access stays in cache. Host state: before `a`, not saved. */
  uint8_t dspWatchLog[32];
  uint8_t dspWatchSync[32];
  /* Pages whose copy in the host's batch view is current (an asynchronous
   * batch host, SNESRECOMP_DSP_LAUNCH_HOOK, sets them; any write to the page
   * clears its bit: apu_dspViewStale for writes outside the SPC). */
  uint8_t dspViewFresh[32];
#endif
  // registers
  uint8_t a;
  uint8_t x;
  uint8_t y;
  uint8_t sp;
  uint16_t pc;
  // flags
  bool c;
  bool z;
  bool v;
  bool n;
  bool i;
  bool h;
  bool p;
  bool b;
  // stopping
  bool stopped;
  // internal use
  uint8_t cyclesUsed; // indicates how many cycles an opcode used
};

Spc* spc_init(Apu* apu);
void spc_free(Spc* spc);
void spc_reset(Spc* spc);
int spc_runOpcode(Spc* spc);
void spc_saveload(Spc *spc, SaveLoadInfo *sli);

#endif
