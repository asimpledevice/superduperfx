/* Test-only: once a second, print progress counters (finds where a run stalls
 * without adding code to the paths it watches). Built with EIP_WATCHDOG=1. */
#include <libdragon.h>
#include "red.h"
extern uint32_t g_apu_stream_posts, g_apu_stream_waits, g_snesdsp_rsp_stream_posts;
extern uint32_t g_snesdsp_rsp_fallbacks, g_red_messages;
static void tick(int ovfl) {
  (void)ovfl;
  debugf("[wd] posts %lu waits %lu rsp posts %lu done %lu msgs %lu fallbacks %lu SP %08lx DP %08lx\n",
         (unsigned long)g_apu_stream_posts, (unsigned long)g_apu_stream_waits,
         (unsigned long)g_snesdsp_rsp_stream_posts, (unsigned long)g_red_dsp_done,
         (unsigned long)g_red_messages, (unsigned long)g_snesdsp_rsp_fallbacks,
         (unsigned long)*SP_STATUS, (unsigned long)*DP_STATUS);
}
static void arm(int ovfl) { (void)ovfl; tick(0); new_timer(TIMER_TICKS(2000000), TF_CONTINUOUS, tick); }
void eip_watchdog_start(void) {
  timer_init();
  /* Nothing until EIP_WATCHDOG_DELAY seconds (not to disturb what it watches). */
#ifndef EIP_WATCHDOG_DELAY
#define EIP_WATCHDOG_DELAY 1
#endif
  new_timer(TIMER_TICKS(1000000LL * EIP_WATCHDOG_DELAY), TF_ONE_SHOT, arm);
}
