# Red mailbox test, overlay B: posts the inverted down word (n64/redtest).
#include <rsp_queue.inc>

    .set noreorder
    .set at

    .data
RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand RedBCmd_Echo, 4
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
SAVED_UNUSED: .ds.b 8
RSPQ_EndSavedState

    .text

RedBCmd_Echo:
    lw t1, %lo(RSPQ_MAILBOX_DOWN)
    nor t1, t1, zero
    RSPQ_MailboxPost t1, t0
    j RSPQ_Loop
    nop
