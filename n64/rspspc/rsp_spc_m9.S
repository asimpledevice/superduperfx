# SPC700 on the RSP, paged module 9: the end of a budget slice. The CPU gives
# the run its port writes (offsets into the run, spc_rsp_layout.h SPCRUN_EVS)
# and a budget up to the first; here, with the budget used up, the next write
# goes to the input ports, as apu.c applies a due write before the next
# instruction, and the budget and END grow to the next offset (or to the
# run's total). With none left, the run ends (C_done). See rsp_spc_m0.S for
# paged modules.
#include <rsp.inc>
#include "spc_rsp_layout.h"
#include "rsp_spc_syms.inc"
#include "rsp_spc_ext_syms.inc"

    .set noreorder
    .set at

    .data
    .ds.b 8                         # (the ucode build wants a data segment; unused)

    .text
    .org EXT_SLOT                   # linked where it runs; only the slot is copied

m9:
    lbu t0, %lo(C_ST + SPCRUN_NEV)
    beqz t0, end
    addiu t0, t0, -1
    sb t0, %lo(C_ST + SPCRUN_NEV)
    lbu t1, %lo(C_ST + SPCRUN_IEV)
    sll t2, t1, 3
    addiu t1, t1, 1
    sb t1, %lo(C_ST + SPCRUN_IEV)
    lbu t3, %lo(C_ST + SPCRUN_EVS + 4)(t2)   # port
    lbu t5, %lo(C_ST + SPCRUN_EVS + 5)(t2)   # value
    sb t5, %lo(C_ST + SPCST_INPORTS)(t3)
    lw t3, %lo(C_ST + SPCRUN_EVS)(t2)        # this offset
    bnez t0, 1f
    lw t5, %lo(C_ST + SPCRUN_EVS + 8)(t2)    # the next one's
    lw t5, %lo(C_ST + SPCRUN_TOTAL)          # none left: the whole budget
1:  subu t5, t5, t3                          # the budget grows by this
    lw t0, %lo(C_ST + SPCST_BUDGET)
    addu t0, t0, t5
    sw t0, %lo(C_ST + SPCST_BUDGET)
    lw t0, %lo(C_END)
    addu t0, t0, t5
    sw t0, %lo(C_END)
    addu fp, fp, t5
    sw zero, %lo(C_ST + SPCST_UNDOFP)        # a trap's undo is within one instruction
    lbu t0, %lo(C_ST + SPCST_STOPPED)        # stopped: single-cycle steps use it all
    beqz t0, 2f
    nop
    blez fp, 2f
    nop
    move fp, zero
2:  j C_loop
    nop
end:
    j C_done
    nop
