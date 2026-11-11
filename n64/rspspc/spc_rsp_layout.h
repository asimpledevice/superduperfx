/* SpcRspState: the SPC700 state block shared by the RSP microcode
 * (rsp_spc.S) and the C glue (spc_rsp.c). Plain #defines so the assembler
 * can include it. Big-endian, naturally aligned. */
#ifndef SPC_RSP_LAYOUT_H
#define SPC_RSP_LAYOUT_H

#define SPCST_A        0x00   /* u8 */
#define SPCST_X        0x01   /* u8 */
#define SPCST_Y        0x02   /* u8 */
#define SPCST_SP       0x03   /* u8 */
#define SPCST_PC       0x04   /* u16 */
#define SPCST_PSW      0x06   /* u8: N V P B H I Z C */
#define SPCST_STOPPED  0x07   /* u8 */
#define SPCST_ARAM     0x08   /* u32: RDRAM address of the 64 KB of ARAM */
#define SPCST_BUDGET   0x0c   /* s32: APU cycles to run (as apu_runCycles) */
#define SPCST_USED     0x10   /* u32 out: cycles used */
#define SPCST_COUNT    0x14   /* u32 out: instructions executed */
#define SPCST_EXIT     0x18   /* u32 out: SPCEXIT_* */
#define SPCST_CCL      0x1c   /* u8: cycles left of the instruction in progress */
#define SPCST_DSPADR   0x1d   /* u8: $F2 */
#define SPCST_ROMREADABLE 0x1e /* u8: IPL ROM mapped at $FFC0 */
#define SPCST_INPORTS  0x20   /* u8[6]: $F4-$F7 from the CPU, $F8-$F9 */
#define SPCST_OUTPORTS 0x26   /* u8[4]: $F4-$F7 to the CPU */
#define SPCST_EXT      0x2c   /* u32: RDRAM: rsp_spc_ext.S code, then a save area
                                 for the rspq IMEM it replaces during the run */
#define SPCST_TIMERS   0x30   /* 3 x 8: cycles, divider, target, counter, enabled */
#define SPCST_CYCLES   0x48   /* u32: APU cycles (apu->cycles) */
#define SPCST_TSYNC    0x4c   /* u32: cycle the timers are current to */
#define SPCST_DSPREGS  0x50   /* u8[128]: DSP register file as the SPC reads it */
#define SPCST_SIZE     0xd0

#define SPCEXIT_BUDGET 0      /* the budget is used up */
#define SPCEXIT_TRAP   1      /* stopped before an opcode the RSP leaves to the CPU */

#endif
