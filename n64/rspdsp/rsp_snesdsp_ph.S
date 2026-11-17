# S-DSP microcode, pitch modulation as HLE: rsp_snesdsp.S with
# RSPDSP_PMOD_HLE (a modulated voice takes the previous voice's output of
# the previous sample; dsp.c with SNESRECOMP_DSP_PMOD_HLE is its reference).
# The build-time alternative to the voice-major variant (rsp_snesdsp_vm.S,
# exact, the LLE reference) for batches with modulated voices.
#define RSPDSP_PMOD_HLE 1
#include "rsp_snesdsp.S"
