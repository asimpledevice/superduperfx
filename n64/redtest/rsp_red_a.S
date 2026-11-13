# Red mailbox test, overlay A (n64/redtest). See red_main.c.
#include <rsp_queue.inc>

    .set noreorder
    .set at

    .data
RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand RedACmd_Post, 8      # post a1 (a0: the command word)
    RSPQ_DefineCommand RedACmd_Serve, 4     # answer a0 (24 bits) new down words with down + 1
    RSPQ_DefineCommand RedACmd_Echo, 4      # post the down word
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
SAVED_UNUSED: .ds.b 8
RSPQ_EndSavedState

    .text

RedACmd_Post:
    RSPQ_MailboxPost a1, t0
    j RSPQ_Loop
    nop

# A long job that talks to the CPU while it runs: for each new down word,
# post down + 1, waiting while the CPU has not taken the last answer.
RedACmd_Serve:
    sll s0, a0, 8
    srl s0, s0, 8                           # the count: a0's low 24 bits
    lw s1, %lo(RSPQ_MAILBOX_DOWN)           # the word before the first new one
1:  beqz s0, 9f
    nop
2:  lw t1, %lo(RSPQ_MAILBOX_DOWN)
    beq t1, s1, 2b
    nop
    move s1, t1
    addiu t2, t1, 1
3:  RSPQ_MailboxTryPost t2, t0
    bnez t0, 3b
    nop
    b 1b
    addiu s0, s0, -1
9:  j RSPQ_Loop
    nop

RedACmd_Echo:
    lw t1, %lo(RSPQ_MAILBOX_DOWN)
    RSPQ_MailboxPost t1, t0
    j RSPQ_Loop
    nop
