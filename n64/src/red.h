/* Red (the RSP kernel, extended rspq) -> CPU messages: the mailbox's up
 * words (rspq_mailbox_set_handler), type in the top byte, payload below.
 * red.c dispatches them in the SP interrupt. Plain C: included by the
 * RSP glue and the platform layer. */
#ifndef N64_RED_H
#define N64_RED_H

#include <stdint.h>

#define RED_MSG(type, payload) (((uint32_t)(type) << 24) | ((uint32_t)(payload) & 0xffffffu))
#define RED_MSG_DSP_DONE 0x01u   /* an S-DSP batch is done; payload: its sequence number */

/* Payload of the last RED_MSG_DSP_DONE: batches up to it are done. */
extern volatile uint32_t g_red_dsp_done;
/* Set by a Blue thread waiting for an S-DSP batch: its sequence number with
 * bit 31 set. The dispatcher clears it and asks Green to hand the CPU back
 * (RUN_FLAG_WANT_BLUE) once that batch is done. */
extern volatile uint32_t g_red_blue_waits;
extern uint32_t g_red_messages;

/* Whether batch `seq` (24-bit, wrapping) is done. */
static inline int red_dsp_done(uint32_t seq) {
  return ((g_red_dsp_done - seq) & 0xffffffu) < 0x800000u;
}

/* Install the dispatcher (after rspq_init). */
void red_init(void);

#endif
