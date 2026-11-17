# S-DSP microcode, voice-major variant: rsp_snesdsp.S with RSPDSP_VM, for
# batches with pitch modulation (docs/n64/RED_DESIGN.md, "The voice-major
# overlay for pitch modulation"). Each voice runs a chunk of samples before
# the next voice, so a modulated voice sees the previous voice's outputs;
# the mixing, echo and output stages are the main variant's.
#define RSPDSP_VM 1
#include "rsp_snesdsp.S"
