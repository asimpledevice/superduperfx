/* Scene builder on the RSP (rsp_scene.S): queue one background pass (a
 * layer, a priority, rows [r0, r1), span [sx0, sx1)) whose tiles the RSP
 * draws from the tile cache straight into rdpq's stream. The caller has
 * set copy mode, loaded the TLUT and set the scissor; the VRAM image and
 * the cache must stay unchanged until the RDP has drawn the pass. */
#ifndef N64_SCENE_RSP_H
#define N64_SCENE_RSP_H

#include <stdint.h>

void n64_scene_rsp_init(void);
/* Times a scene command found too little RDP buffer room and went back to
 * the dispatch loop (which serves the ring first) to try again. */
uint32_t n64_scene_rsp_room_waits(void);
void n64_scene_rsp_bg(const uint16_t *vram, const uint8_t *cache, const uint8_t *empty,
                      uint16_t hs, uint16_t vs,
                      uint8_t xsc, int bpp, int big, uint16_t tileadr, int r0, int r1, int prio,
                      int sx0, int sx1, int ox, int oy);
/* Queue a list of n tile records (scene_layout.h, ST_REC; at most
 * SCENE_TILES_MAX, 8-aligned, written back from the data cache, with
 * 512 readable bytes from `records` on) drawn from the CI4 cache `cache`,
 * under the same conditions as n64_scene_rsp_bg. */
void n64_scene_rsp_tiles(const void *records, int n, const uint8_t *cache, const uint8_t *empty,
                         int ox, int oy);

#endif
