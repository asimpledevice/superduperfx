/* The per-line sprite table (ppu_evaluateSpritesFromLines) must leave the
 * same range-over/time-over flags and line-has-sprites result as the full
 * evaluation. Built with SNESRECOMP_SPR_LINES_CHECK, ppu.c runs both on
 * every line the table answers and aborts on any difference; this harness
 * drives random sprite tables through both validation modes (content copy,
 * and oamWriteCount for a tracked PPU written through the ports).
 * No game ROM or platform frontend is required. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
extern unsigned long g_ppu_spr_lines_checked;

uint16_t WsShadowTile(int layer, int screen_x, uint32_t wrapped_y, uint16_t real_tile) {
    (void)layer; (void)screen_x; (void)wrapped_y;
    return real_tile;
}
bool WsShadowLayerActive(int layer) { (void)layer; return false; }
uint32_t WsShadowWorldX(int layer) { (void)layer; return 0; }
uint32_t WsShadowPresentWorldY(int layer, int screen_x) { (void)layer; (void)screen_x; return 0; }
uint32_t WsShadowScrollY(int layer) { (void)layer; return 0; }
void WsShadowOnVramWrite(uint16_t word_adr, uint16_t value) { (void)word_adr; (void)value; }

static uint32_t s_rng = 0x12345678u;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

/* Random OAM; crowded tables put many sprites near a few rows and x
 * positions so both the 32-sprite and 34-sliver limits trigger. */
static void random_oam(uint16_t oam[256], uint8_t high[32], int crowd) {
    const int y0 = (int)(rnd() % 256), x0 = (int)(rnd() % 512);
    for (int s = 0; s < 128; s++) {
        int y, x;
        if (crowd && rnd() % 4) {
            y = (y0 + (int)(rnd() % 24)) & 255;
            x = (x0 + (int)(rnd() % 96) - 48) & 511;
        } else {
            y = (int)(rnd() % 256);
            x = (int)(rnd() % 512);
        }
        oam[s * 2] = (uint16_t)((y << 8) | (x & 0xff));
        oam[s * 2 + 1] = (uint16_t)rnd();
        const int bit = (s & 3) * 2;
        high[s >> 2] = (uint8_t)((high[s >> 2] & ~(3u << bit)) |
                                 (unsigned)(((x >> 8) & 1) | ((rnd() & 1) << 1)) << bit);
    }
}

static void set_mode(Ppu *ppu) {
    ppu_write(ppu, 0x00, 0x0f);                                   /* display on */
    ppu_write(ppu, 0x01, (uint8_t)((rnd() % 8) << 5));            /* sprite sizes */
    ppu_write(ppu, 0x33, (uint8_t)((rnd() & 1) ? 0x02 : 0x00));   /* OBJ interlace */
    ppu_write(ppu, 0x02, (uint8_t)rnd());                         /* rotation start */
    ppu_write(ppu, 0x03, (uint8_t)(rnd() & 0x80));
}

static void run_frame(Ppu *ppu) {
    ppu_runLineState(ppu, 0);
    for (int line = 1; line <= 224; line++) ppu_runLineState(ppu, line);
}

int main(void) {
    Ppu *ppu = ppu_init();
    ppu_reset(ppu);
    uint16_t oam[256];
    uint8_t high[32];

    /* Content-validated: OAM written directly between frames and mid-frame. */
    for (int frame = 0; frame < 400; frame++) {
        set_mode(ppu);
        memset(high, 0, sizeof high);
        random_oam(oam, high, frame & 1);
        memcpy(ppu->oam, oam, sizeof oam);
        memcpy(ppu->highOam, high, sizeof high);
        ppu_runLineState(ppu, 0);
        for (int line = 1; line <= 224; line++) {
            if (line == 100 && frame % 3 == 0) ppu->oam[(rnd() % 128) * 2] ^= 0x2000;
            ppu_runLineState(ppu, line);
        }
    }
    const unsigned long content = g_ppu_spr_lines_checked;

    /* Counter-validated: OAM written through $2102-$2104 only. */
    ppu_set_oam_write_tracking(ppu);
    for (int frame = 0; frame < 400; frame++) {
        set_mode(ppu);
        memset(high, 0, sizeof high);
        random_oam(oam, high, frame & 1);
        ppu_write(ppu, 0x02, 0);
        ppu_write(ppu, 0x03, 0);
        for (int i = 0; i < 256; i++) {
            ppu_write(ppu, 0x04, (uint8_t)oam[i]);
            ppu_write(ppu, 0x04, (uint8_t)(oam[i] >> 8));
        }
        for (int i = 0; i < 32; i++) ppu_write(ppu, 0x04, high[i]);
        run_frame(ppu);
    }
    const unsigned long tracked = g_ppu_spr_lines_checked - content;
    ppu_set_oam_write_tracking(NULL);
    ppu_free(ppu);

    printf("sprite line table: %lu lines (content), %lu lines (tracked) matched\n",
           content, tracked);
    if (content < 10000 || tracked < 10000) {
        fprintf(stderr, "FAIL: the line table answered too few lines\n");
        return 1;
    }
    return 0;
}
