/* RspDspState layout, shared by the RSP microcode and the C glue. Plain
 * #defines so the assembler can include it. All values are big-endian.
 *
 * Per-voice values are 8 x int16 vectors (one lane per voice). The decode
 * buffers hold each voice's 19 samples doubled (2 * s; BRR output is 15-bit,
 * so this is exact), the form the interpolation multiply wants. */
#ifndef SNESDSP_RSP_LAYOUT_H
#define SNESDSP_RSP_LAYOUT_H

/* Per-voice vectors (16 bytes each). */
#define RSPDSP_V_PITCH      0x000
#define RSPDSP_V_COUNTER    0x010
#define RSPDSP_V_GAIN       0x020
#define RSPDSP_V_SAMPLEOUT  0x030
#define RSPDSP_V_VOLL       0x040  /* int8 sign-extended */
#define RSPDSP_V_VOLR       0x050
#define RSPDSP_V_RATECTR    0x060
#define RSPDSP_V_GAINVAL    0x070
#define RSPDSP_V_SUSTAIN    0x080
#define RSPDSP_V_DECOFF     0x090
#define RSPDSP_V_OLD        0x0a0
#define RSPDSP_V_OLDER      0x0b0
#define RSPDSP_V_RATE0      0x0c0  /* adsrRates[0..3] */
#define RSPDSP_V_RATE1      0x0d0
#define RSPDSP_V_RATE2      0x0e0
#define RSPDSP_V_RATE3      0x0f0
#define RSPDSP_V_SRCN       0x100
#define RSPDSP_V_PREVFLAGS  0x110
#define RSPDSP_V_ADSRSTATE  0x120
#define RSPDSP_V_GAINMODE   0x130
#define RSPDSP_DECODE       0x140  /* 8 voices x 20 int16 (19 used), doubled */
#define RSPDSP_DECODE_STRIDE 40
#define RSPDSP_FIR_L        0x280  /* 16 int16: history twice, for rotation */
#define RSPDSP_FIR_R        0x2a0
#define RSPDSP_FIR_VALS     0x2c0  /* 8 int16, int8 sign-extended */
#define RSPDSP_RAM          0x2d0  /* 128 bytes: the DSP register file */

/* Globals (int16 unless noted). */
#define RSPDSP_G            0x350
#define RSPDSP_DIRPAGE      (RSPDSP_G + 0x00)
#define RSPDSP_EVENCYCLE    (RSPDSP_G + 0x02)
#define RSPDSP_MUTE         (RSPDSP_G + 0x04)
#define RSPDSP_RESET        (RSPDSP_G + 0x06)
#define RSPDSP_MVOLL        (RSPDSP_G + 0x08)
#define RSPDSP_MVOLR        (RSPDSP_G + 0x0a)
#define RSPDSP_NOISE_SAMPLE (RSPDSP_G + 0x0c)
#define RSPDSP_NOISE_RATE   (RSPDSP_G + 0x0e)
#define RSPDSP_NOISE_COUNTER (RSPDSP_G + 0x10)
#define RSPDSP_ECHOWRITES   (RSPDSP_G + 0x12)
#define RSPDSP_EVOLL        (RSPDSP_G + 0x14)
#define RSPDSP_EVOLR        (RSPDSP_G + 0x16)
#define RSPDSP_FEEDBACK     (RSPDSP_G + 0x18)
#define RSPDSP_ECHOADR      (RSPDSP_G + 0x1a)
#define RSPDSP_ECHODELAY    (RSPDSP_G + 0x1c)
#define RSPDSP_ECHOREMAIN   (RSPDSP_G + 0x1e)
#define RSPDSP_ECHOINDEX    (RSPDSP_G + 0x20)
#define RSPDSP_FIRINDEX     (RSPDSP_G + 0x22)
#define RSPDSP_M_KEYON      (RSPDSP_G + 0x24)  /* masks: bit per voice */
#define RSPDSP_M_KEYOFF     (RSPDSP_G + 0x26)
#define RSPDSP_M_NOISE      (RSPDSP_G + 0x28)
#define RSPDSP_M_PMOD       (RSPDSP_G + 0x2a)
#define RSPDSP_M_ECHO       (RSPDSP_G + 0x2c)
#define RSPDSP_M_USEGAIN    (RSPDSP_G + 0x2e)
#define RSPDSP_M_DIRECTGAIN (RSPDSP_G + 0x30)
/* Batch parameters, set by the CPU for each run. */
#define RSPDSP_P_SAMPLES    (RSPDSP_G + 0x32)
#define RSPDSP_P_LOGN       (RSPDSP_G + 0x34)
#define RSPDSP_P_ARAM       (RSPDSP_G + 0x38)  /* u32 RDRAM address */
#define RSPDSP_P_LOG        (RSPDSP_G + 0x3c)  /* u32 RDRAM address */
#define RSPDSP_P_OUT        (RSPDSP_G + 0x40)  /* u32 RDRAM address */
/* Coefficients split for exact vector multiplies: for a coefficient k
 * scaled to K = k << (16 - shift), u = K & 0xffff (unsigned) and
 * c = K >> 16 (signed), so VMUDM(s, u) + VMADH(s, c) leaves exactly
 * floor(s * k / 2^shift) in the accumulator (shift 6 for voice volumes and
 * FIR taps, 7 for master/echo volume and feedback). Kept in step with the
 * coefficients by the CPU (pack and decoded register writes). */
#define RSPDSP_V_VOLL_U     0x3a0
#define RSPDSP_V_VOLL_C     0x3b0
#define RSPDSP_V_VOLR_U     0x3c0
#define RSPDSP_V_VOLR_C     0x3d0
#define RSPDSP_V_ECHOMASK   0x3e0  /* 0xffff where echo is enabled */
#define RSPDSP_FIR_U        0x3f0
#define RSPDSP_FIR_C        0x400
#define RSPDSP_SC_U         0x410  /* lanes: mvolL mvolR evolL evolR fb fb */
#define RSPDSP_SC_C         0x420
#define RSPDSP_STATE_SIZE   0x430  /* multiple of 16 */

/* Log entries (8 bytes): u16 tag (samples owed when written), u8 kind,
 * u8 unused, u16 address, u16 value. Entries are in time order. Kinds:
 *   0 ARAM byte write (address = ARAM address)
 *   1 state halfword store (address = offset in RspDspState)
 *   2 state byte store (address = offset in RspDspState)
 * DSP register writes are decoded on the CPU by running dsp_write on a
 * shadow copy and emitting stores of every field it assigns. */
#define RSPDSP_LOG_ENTRY    8
#define RSPDSP_LOG_ARAM     0
#define RSPDSP_LOG_HALF     1
#define RSPDSP_LOG_BYTE     2

/* The ARAM view the RSP reads is 64 KB plus a 16-byte mirror of its first
 * 16 bytes, so a BRR block that wraps past $FFFF reads contiguously. */
#define RSPDSP_ARAM_SIZE    (0x10000 + 16)

#endif
