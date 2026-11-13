/* Red's messages to the CPU (red.h): the mailbox doorbell's handler, run
 * in the SP interrupt. */
#include <libdragon.h>
#include "red.h"
#include "run_flags.h"

volatile uint32_t g_red_dsp_done, g_red_blue_waits;
uint32_t g_red_messages;

static void red_dispatch(uint32_t up) {
  g_red_messages++;
  switch (up >> 24) {
    case RED_MSG_DSP_DONE:
      g_red_dsp_done = up & 0xffffffu;
      if (g_red_blue_waits && red_dsp_done(g_red_blue_waits & 0xffffffu)) {
        g_red_blue_waits = 0;
        run_flags_set_in_interrupt(RUN_FLAG_WANT_BLUE);
      }
      break;
    default:
      break;
  }
}

void red_init(void) { rspq_mailbox_set_handler(red_dispatch); }
