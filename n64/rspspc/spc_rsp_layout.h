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
#define SPCST_LOGGING  0x14   /* u8: a DSP batch is open: log ARAM and DSP writes */
#define SPCST_EXIT     0x18   /* u32 out: SPCEXIT_* */
#define SPCST_CCL      0x1c   /* u8: cycles left of the instruction in progress */
#define SPCST_DSPADR   0x1d   /* u8: $F2 */
#define SPCST_ROMREADABLE 0x1e /* u8: IPL ROM mapped at $FFC0 */
#define SPCST_INPORTS  0x20   /* u8[6]: $F4-$F7 from the CPU, $F8-$F9 */
#define SPCST_OUTPORTS 0x26   /* u8[4]: $F4-$F7 to the CPU */
#define SPCST_TIMERS   0x30   /* 3 x 8: cycles, divider, target, counter, enabled */
#define SPCST_CYCLES   0x48   /* u32: APU cycles (apu->cycles) */
#define SPCST_TSYNC    0x4c   /* u32: cycle the timers are current to */
#define SPCST_DSPREGS  0x50   /* u8[128]: DSP register file as the SPC reads it */
#define SPCST_SYNC     0xd0   /* u8[32]: pages whose reads need the DSP current (echo) */
#define SPCST_RAWBUF   0xf0   /* u32: RDRAM: raw log of the run (SpcRspLog entries) */
#define SPCST_UNDOFP   0xf4   /* u32: FP0 of the last instruction to read a timer, 0 none;
                                 * the counters before it in each timer's pad[1] */
#define SPCST_RAWN     0xf8   /* u32 out: raw log written, bytes */
#define SPCST_DIRPAGE  0xfc   /* u16: DSP sample directory (writes there trap) */
#define SPCST_SIZE     0x100

/* Raw log entry (8 bytes, as ApuDspLogEntry but with the instruction's
 * cycle offset in the run where the sample tag goes; spc_rsp_run.c turns
 * it into the tag and keeps what the C core would have logged). */
/* The run's scratch, after the block in borrowed DMEM (offsets from the
 * block's DMEM address, ST in rsp_spc.S). */
#define SPCLOG_STAGE   0x100  /* 64-byte staging buffer for the raw log */
#define SPCRUN_SP0     0x140  /* SP and PSW at the instruction's start */
#define SPCRUN_PSW0    0x141
#define SPCRUN_MOD     0x142  /* paged module in the slot (0xff none: the CPU sets it) */
#define SPCRUN_RA      0x144  /* ra across a module load */
#define SPCRUN_TAGS    0x150  /* 64 bytes: the line cache's tags (bit 15 dirty) */
#define SPCRUN_WPAGES  0x190  /* 32 bytes: pages of dirty lines evicted during the run */
/* CPU port writes the run applies itself (module 9): the run's budget ends
 * at each one's offset; there the write goes to the input ports and the
 * budget (and END) grow to the next offset, or to the total after the last. */
#define SPCRUN_NEV     0x1b0  /* u8: writes left */
#define SPCRUN_IEV     0x1b1  /* u8: writes applied */
#define SPCRUN_TOTAL   0x1b4  /* u32: the run's whole budget */
#define SPCRUN_EVS     0x1b8  /* 6 x {u32 offset, u8 port, u8 value, u16 pad} */
#define SPCRUN_MAXEV   6
#define SPCRUN_IO      0x1e8  /* bytes moved each way: the block and the run's
                               * area, so the CPU zeroes the tags and the page
                               * bits and reads which pages the run wrote */

/* Paged modules (rsp_spc_m<k>.S): linked at EXT_SLOT in the borrowed IMEM,
 * after the resident borrowed code (rsp_spc_ext.S); kept in the RDRAM ext
 * buffer at EXT_MOD_OFS + k * EXT_MOD_SIZE and DMA'd into the slot when an
 * operation needs one that is not there. */
#define EXT_SLOT       0x1c0
#define EXT_MOD_SIZE   0x100
#define EXT_MOD_OFS    0x840
#define EXT_MODS       10     /* 0-4, 7: opcodes the core has no room for, 6: DIV, 8: $F1 writes,
                               * 9: the end of a budget slice (port writes) */

#define SPCEXIT_BUDGET 0      /* the budget is used up */
#define SPCEXIT_TRAP   1      /* stopped before an opcode the RSP leaves to the CPU */

#endif
