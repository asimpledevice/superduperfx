# S-DSP microcode, no-echo variant: rsp_snesdsp.S built without the echo
# path, for batches where echo writes are off and both echo volumes are 0
# throughout (see Echo under RSPDSP_NO_ECHO there, and choose_overlay in
# snesdsp_rsp.c). Leaves IMEM free for code the full variant cannot hold.
#define RSPDSP_NO_ECHO 1
#include "rsp_snesdsp.S"
