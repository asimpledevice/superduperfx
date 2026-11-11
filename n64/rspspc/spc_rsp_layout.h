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
#define SPCST_BUDGET   0x0c   /* s32: run until this many cycles are used */
#define SPCST_USED     0x10   /* u32 out: cycles used */
#define SPCST_COUNT    0x14   /* u32 out: instructions executed */
#define SPCST_EXIT     0x18   /* u32 out: SPCEXIT_* */
#define SPCST_SIZE     0x20

#define SPCEXIT_BUDGET 0      /* the budget is used up */
#define SPCEXIT_TRAP   1      /* stopped before an opcode the RSP leaves to the CPU */

#endif
