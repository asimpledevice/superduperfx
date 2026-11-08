/* RspDspState byte offsets, shared by the RSP microcode and the C glue.
 * Plain #defines so the assembler can include it. */
#ifndef SNESDSP_RSP_LAYOUT_H
#define SNESDSP_RSP_LAYOUT_H

#define RSPDSP_NOISE_SAMPLE   0x00  /* int16 */
#define RSPDSP_NOISE_RATE     0x02  /* uint16 */
#define RSPDSP_NOISE_COUNTER  0x04  /* uint16 */
#define RSPDSP_STATE_SIZE     0x10  /* multiple of 16 */

#endif
