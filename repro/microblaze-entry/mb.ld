/* petalogix-s3adsp1800: DDR at 0x90000000, 256 MB in the QEMU model.  */
ENTRY(_start)
MEMORY
{
  ram (rwx) : ORIGIN = 0x90000000, LENGTH = 64M
}
SECTIONS
{
  .text : { *(.text .text.*) } > ram
  .rodata : { *(.rodata .rodata.*) } > ram
  .data : { *(.data .data.*) *(.sdata .sdata.*) } > ram
  .bss : { __bss_start = .; *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) __bss_end = .; } > ram
  . = ALIGN(16);
  . = . + 0x10000;
  _stack_top = .;
  /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
