/* Scene builder on the RSP (rsp_scene.S): queue one background pass (a
 * layer, a priority, rows [r0, r1), span [sx0, sx1)) whose tiles the RSP
 * draws from the tile cache straight into rdpq's stream. The caller has
 * set copy mode, loaded the TLUT and set the scissor; the VRAM image and
 * the cache must stay unchanged until the RDP has drawn the pass. */
#ifndef N64_SCENE_RSP_H
#define N64_SCENE_RSP_H

#include <stdint.h>

void n64_scene_rsp_init(void);
void n64_scene_rsp_bg(const uint16_t *vram, const uint8_t *cache, uint16_t hs, uint16_t vs,
                      uint8_t xsc, int bpp, uint16_t tileadr, int r0, int r1, int prio,
                      int sx0, int sx1, int ox, int oy);

#endif
