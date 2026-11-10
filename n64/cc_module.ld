/* One code-cache module, linked on its own at the code cache's link base
 * (tools/n64/pack_code_cache.py). The base lies in kseg0, the segment the
 * runtime lives in, so jal to runtime functions encodes as at run time; the
 * loader moves the module by adding (load address - base) at each of its
 * internal relocations. */
SECTIONS {
  /* 0x80000000, written sign-extended as the 64-bit linker sees kseg0
   * symbols of the runtime (--just-symbols); unextended, jal to them would
   * not fit. */
  . = 0xFFFFFFFF80000000;
  .text : { *(.text .text.*) }
  .rodata : ALIGN(8) { *(.rodata .rodata.* .rodata1) }
  .data : ALIGN(8) { *(.data .data.* .sdata .sdata.*) }
  .bss : ALIGN(8) { *(.bss .bss.* .sbss .sbss.* COMMON) }
  /DISCARD/ : { *(.comment) *(.note .note.*) *(.MIPS.abiflags) *(.reginfo) *(.pdr)
                *(.gnu.attributes) *(.eh_frame) *(.gcc_compiled_long32) }
}
