# Red mailbox test, overlay B: posts the inverted down word (n64/redtest).
#include <rsp_queue.inc>

    .set noreorder
    .set at

    .data
RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand RedBCmd_Echo, 4
    RSPQ_DefineCommand RedBCmd_Add, 4       # SUM += a0's low 24 bits
    RSPQ_DefineCommand RedBCmd_Report, 4    # post SUM
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
SUM: .long 0, 0
RSPQ_EndSavedState

    .text

RedBCmd_Echo:
    lw t1, %lo(RSPQ_MAILBOX_DOWN)
    nor t1, t1, zero
    RSPQ_MailboxPost t1, t0
    j RSPQ_Loop
    nop

RedBCmd_Add:
    sll a0, 8
    srl a0, 8
    lw t1, %lo(SUM)
    addu t1, a0
    j RSPQ_Loop
    sw t1, %lo(SUM)

RedBCmd_Report:
    lw t1, %lo(SUM)
    RSPQ_MailboxPost t1, t0
    j RSPQ_Loop
    nop
