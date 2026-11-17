# S-DSP microcode, pitch-modulation variant: rsp_snesdsp.S with RSPDSP_PM,
# for batches with voices untracked by pitch modulation (docs/n64/
# RED_DESIGN.md, "The pitch-modulation variant"): the sample-major
# microcode, every voice read from ARAM through the ledger, and a repair
# pass for the modulated voices each sample.
#define RSPDSP_PM 1
#include "rsp_snesdsp.S"
