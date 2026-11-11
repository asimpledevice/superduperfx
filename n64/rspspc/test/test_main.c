/* RSP SPC700 test ROM: libdragon side (the tests are in test_ref.c). */
#include <libdragon.h>
#include "spc_rsp.h"
void run_tests(void);
uint32_t test_ticks(void) { return get_ticks(); }
int main(void) {
  debug_init_isviewer();
  rspq_init();
  spc_rsp_init();
  run_tests();
  for (;;) {}
}
