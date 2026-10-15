/* Linker script for RetroPixels' GBA test ROMs (original code). */
ENTRY(_start)
MEMORY { ROM (rx) : ORIGIN = 0x08000000, LENGTH = 32M
         IWRAM (rwx) : ORIGIN = 0x03000000, LENGTH = 0x7000
         EWRAM (rwx) : ORIGIN = 0x02000000, LENGTH = 256K }
SECTIONS {
  .text : { *(.text.start) *(.text*) *(.rodata*) } > ROM
  .data : { __data_start = .; *(.iwram*) *(.data*) . = ALIGN(4); __data_end = .; } > IWRAM AT > ROM
  __data_lma = LOADADDR(.data);
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > IWRAM
  .ewram (NOLOAD) : { *(.ewram*) } > EWRAM
  /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
